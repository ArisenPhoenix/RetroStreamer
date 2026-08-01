#include "host/session_control_monitor.hpp"

#include "common/serialization.hpp"
#include "host/active_session_slot.hpp"
#include "host/host_session_hub.hpp"
#include "host/retroarch_netcmd.hpp"

#include <algorithm>
#include <iostream>
#include <sstream>
#include <variant>

namespace archstreamer {
namespace {

constexpr auto kStartupHeartbeatGrace = std::chrono::seconds(15);
constexpr auto kMinReconfigureInterval = std::chrono::seconds(5);
// After a pipeline restart the remote often reports 0 decoded frames until the next IDR.
// Without this grace, Auto flaps High↔Medium every few seconds and freezes the stream.
constexpr auto kPostReconfigureGrace = std::chrono::seconds(12);
// After a failed High stay, wait before retrying — High restarts the shared capture tee.
constexpr auto kHighTierFailureCooldown = std::chrono::seconds(180);
constexpr std::uint8_t kBadHealthThreshold = 3;
// Demote from High only after a longer bad streak; brief decode stalls are common at 60fps.
constexpr std::uint8_t kBadHealthThresholdFromHigh = 6;
constexpr std::uint8_t kGoodHealthThreshold = 10;           // Low → Medium (~10s)
constexpr std::uint8_t kGoodHealthThresholdForHigh = 45;    // Medium → Med-High (~45s)
constexpr std::uint16_t kHighLossPermille = 100;
// Require sustained decode rate before climbing (heartbeats are ~1 Hz).
constexpr std::uint16_t kMinFramesForStepUp = 8;
constexpr std::uint16_t kMinFramesForHighStepUp = 20;
// Auto must not walk into High/Very-High: each step restarts the shared encode
// tee (~1s full-screen freeze). 1080p60@12–25 Mbps also overloads many Wi‑Fi
// clients. Players can still pick High/Very-High explicitly in the client UI.
constexpr MediaQualityTier kAutoMaxTier = MediaQualityTier::MediumHigh;
constexpr auto kFramecountOsdInterval = std::chrono::milliseconds(500);

bool tier_above(MediaQualityTier a, MediaQualityTier b) {
    const auto rank = [](MediaQualityTier tier) {
        switch (tier) {
        case MediaQualityTier::Low:
            return 0;
        case MediaQualityTier::Medium:
        case MediaQualityTier::Auto:
            return 1;
        case MediaQualityTier::MediumHigh:
            return 2;
        case MediaQualityTier::High:
            return 3;
        case MediaQualityTier::VeryHigh:
            return 4;
        }
        return 0;
    };
    return rank(a) > rank(b);
}

bool client_is_seated_player(const SessionClientConnection& client) {
    return client.hello.requested_players > 0;
}

bool any_connected_seated_player(const SessionPlan& plan) {
    for (const auto& client : plan.clients) {
        if (client_is_seated_player(client) &&
            client.connection_state == SessionConnectionState::Connected) {
            return true;
        }
    }
    return false;
}

std::chrono::seconds reconnect_grace_for(const SessionClientConnection& client, std::chrono::seconds full) {
    // Explicit ClientSessionLeave → end immediately (no reconnect hold).
    // TCP close / heartbeat loss → full reconnect window for flaky links.
    if (client.disconnect_reason == "left") {
        return std::chrono::seconds(0);
    }
    return full;
}

} // namespace

SessionControlMonitor::SessionControlMonitor(
    SessionPlan& plan,
    InputRouter& input_router,
    MediaServer& media_server,
    std::chrono::seconds heartbeat_timeout,
    std::chrono::seconds reconnect_timeout,
    HostSessionHub* host_hub)
    : plan_(plan),
      input_router_(input_router),
      media_server_(media_server),
      heartbeat_timeout_(heartbeat_timeout),
      reconnect_timeout_(reconnect_timeout),
      started_at_(std::chrono::steady_clock::now()),
      host_hub_(host_hub) {
    const auto now = started_at_;
    for (auto& client : plan_.clients) {
        client.last_seen = now;
        // Start at Medium: High is 12 Mbps/60 and restarts the shared pipeline.
        // Auto can still step up after a long healthy streak (see kGoodHealthThresholdForHigh).
        client.applied_tier = MediaQualityTier::Medium;
        client.wanted_tier = MediaQualityTier::Auto;
    }
}

std::optional<std::string> SessionControlMonitor::poll() {
    const auto now = std::chrono::steady_clock::now();
    const auto in_startup_grace = now - started_at_ < kStartupHeartbeatGrace;

    if (plan_.soft_keyboard) {
        // Only consume the request once somebody can actually receive it, otherwise it
        // is marked sent and lost. This replaces the old timed re-publish.
        const bool any_connected = std::any_of(
            plan_.clients.begin(),
            plan_.clients.end(),
            [](const auto& client) {
                return client.connection_state == SessionConnectionState::Connected;
            });
        std::optional<SoftKeyboardRequest> request;
        if (any_connected) {
            request = plan_.soft_keyboard->take_unsent_request();
        }
        if (request.has_value()) {
            for (auto& client : plan_.clients) {
                if (client.connection_state != SessionConnectionState::Connected) {
                    continue;
                }
                try {
                    client.stream.send_packet(serialize_packet(*request));
                    std::cout
                        << "Soft keyboard request id=" << request->request_id
                        << " sent to " << client_label(client) << '\n';
                } catch (const std::exception& error) {
                    std::cerr
                        << "Failed to send SoftKeyboardRequest to "
                        << client_label(client) << ": " << error.what() << '\n';
                }
            }
        }
    }

    for (std::size_t i = 0; i < plan_.clients.size();) {
        auto& client = plan_.clients[i];
        if (client.connection_state == SessionConnectionState::Disconnected) {
            const auto grace = reconnect_grace_for(client, reconnect_timeout_);
            if (now - client.disconnected_at >= grace) {
                // Last seated player gave up reconnecting → end session (host returns to lobby).
                if (!any_connected_seated_player(plan_)) {
                    return client_label(client) + " left; ending session for a new lobby";
                }
                return client_label(client) + " reconnect timed out";
            }
            ++i;
            continue;
        }

        bool removed_current = false;
        while (client.stream.readable()) {
            const auto packet = client.stream.receive_packet();
            if (!packet.has_value()) {
                if (remove_viewer(i, "disconnected")) {
                    removed_current = true;
                    break;
                }
                mark_player_disconnected(client, "disconnected");
                if (!any_connected_seated_player(plan_)) {
                    // Still honor short grace inside the Disconnected branch next poll.
                    ++i;
                    removed_current = true;
                    break;
                }
                ++i;
                removed_current = true;
                break;
            }

            const auto payload = deserialize_packet(*packet);
            if (const auto* leave = std::get_if<ClientSessionLeave>(&payload); leave != nullptr) {
                const auto reason = leave->reason.empty() ? "left" : leave->reason;
                if (remove_viewer(i, reason)) {
                    removed_current = true;
                    break;
                }
                mark_player_disconnected(client, "left");
                if (!any_connected_seated_player(plan_)) {
                    return client_label(client) + " left; ending session for a new lobby";
                }
                ++i;
                removed_current = true;
                break;
            }
            if (const auto* heartbeat = std::get_if<ViewerHeartbeat>(&payload); heartbeat != nullptr) {
                if (heartbeat->client_id == client.client_id) {
                    handle_heartbeat(client, *heartbeat);
                }
            } else if (const auto* disc_request = std::get_if<DiscControlRequest>(&payload);
                       disc_request != nullptr) {
                const auto response = apply_disc_control(plan_, *disc_request);
                try {
                    client.stream.send_packet(serialize_packet(response));
                } catch (const std::exception& error) {
                    std::cerr << "Failed to send DiscControlResponse: " << error.what() << '\n';
                }
                if (response.ok) {
                    std::cout << "Disc control: " << response.message << '\n';
                } else {
                    std::cerr << "Disc control failed: " << response.message << '\n';
                }
            } else if (const auto* soft_keyboard = std::get_if<SoftKeyboardResponse>(&payload);
                       soft_keyboard != nullptr) {
                if (plan_.soft_keyboard) {
                    plan_.soft_keyboard->submit_response(*soft_keyboard);
                    std::cout
                        << "Soft keyboard response id=" << soft_keyboard->request_id
                        << " accepted=" << (soft_keyboard->accepted ? "yes" : "no")
                        << " from " << client_label(client) << '\n';
                }
            } else if (const auto* link_request = std::get_if<LinkRequest>(&payload);
                       link_request != nullptr) {
                std::vector<LinkOutbound> outbound;
                if (host_hub_ != nullptr) {
                    // Need ActiveSessionSlot& — hub looks up from client id.
                    if (auto* slot = host_hub_->slot_for_client(client.client_id);
                        slot != nullptr) {
                        outbound = host_hub_->handle_link(
                            *slot,
                            client.client_id,
                            client.hello.username,
                            *link_request);
                    } else {
                        LinkResponse err;
                        err.status = LinkStatus::Error;
                        err.message = "Link: session slot not registered";
                        outbound.push_back({client.client_id, std::move(err)});
                    }
                } else {
                    outbound = plan_.link_coordinator.handle(
                        plan_,
                        client.client_id,
                        client.hello.username,
                        *link_request);

                    bool started_cable = false;
                    for (auto& item : outbound) {
                        if (!started_cable && item.response.status == LinkStatus::Matched) {
                            started_cable = true;
                            std::string peer_user = item.response.peer_username;
                            ClientId peer_id = 0;
                            for (const auto& other : outbound) {
                                if (other.client_id != item.client_id) {
                                    peer_id = other.client_id;
                                    break;
                                }
                            }
                            if (peer_id == 0) {
                                for (const auto& candidate : plan_.clients) {
                                    if (candidate.client_id != client.client_id &&
                                        candidate.connection_state == SessionConnectionState::Connected &&
                                        candidate.hello.username == peer_user) {
                                        peer_id = candidate.client_id;
                                        break;
                                    }
                                }
                            }
                            const auto start = plan_.link_cable.begin(
                                plan_.system_key,
                                peer_id,
                                client.client_id,
                                peer_user,
                                client.hello.username,
                                assigned_player_count(plan_.seats),
                                false);
                            for (auto& update : outbound) {
                                update.response.message = start.message;
                                if (!start.ok) {
                                    update.response.ok = false;
                                    update.response.status = LinkStatus::Error;
                                }
                            }
                            if (start.ok) {
                                std::cout << "Link cable: " << start.message << '\n';
                                if (start.needs_runtime_promotion) {
                                    plan_.pending_link_promotion = true;
                                    plan_.pending_link_host_client_id = start.logical_host_client_id;
                                    plan_.pending_link_client_client_id = start.logical_client_client_id;
                                    plan_.pending_link_host_username = start.logical_host_username;
                                    plan_.pending_link_client_username = start.logical_client_username;
                                }
#if defined(ARCHSTREAMER_DEBUG_GB_LINK)
                                send_retroarch_netcmd(
                                    std::string("SHOW_MSG ") + "Link cable: dual GB ready",
                                    plan_.retroarch_netcmd_port);
#endif
                            } else {
                                std::cerr << "Link cable: " << start.message << '\n';
                            }
                        }
                    }
                }

                for (const auto& item : outbound) {
                    SessionClientConnection* target = nullptr;
                    if (host_hub_ != nullptr) {
                        if (auto* slot = host_hub_->slot_for_client(item.client_id); slot != nullptr) {
                            for (auto& candidate : slot->plan().clients) {
                                if (candidate.client_id == item.client_id &&
                                    candidate.connection_state == SessionConnectionState::Connected) {
                                    target = &candidate;
                                    break;
                                }
                            }
                        }
                    } else {
                        for (auto& candidate : plan_.clients) {
                            if (candidate.client_id == item.client_id &&
                                candidate.connection_state == SessionConnectionState::Connected) {
                                target = &candidate;
                                break;
                            }
                        }
                    }
                    if (target == nullptr) {
                        continue;
                    }
                    try {
                        target->stream.send_packet(serialize_packet(item.response));
                    } catch (const std::exception& error) {
                        std::cerr << "Failed to send LinkResponse: " << error.what() << '\n';
                    }
                    if (item.response.ok) {
                        std::cout
                            << "Link: client " << static_cast<int>(item.client_id)
                            << " " << item.response.message << '\n';
                    } else {
                        std::cerr
                            << "Link failed (client " << static_cast<int>(item.client_id)
                            << "): " << item.response.message << '\n';
                    }
                }
            }
        }
        if (removed_current) {
            continue;
        }

        if (client.stream.peer_closed()) {
            if (remove_viewer(i, "disconnected")) {
                continue;
            }
            mark_player_disconnected(client, "disconnected");
            ++i;
            continue;
        }
        if (!in_startup_grace && now - client.last_seen > heartbeat_timeout_) {
            if (remove_viewer(i, "heartbeat timed out")) {
                continue;
            }
            mark_player_disconnected(client, "heartbeat timed out");
            ++i;
            continue;
        }
        ++i;
    }

    bool want_framecount = false;
    for (const auto& client : plan_.clients) {
        if (client.connection_state == SessionConnectionState::Connected && client.show_framecount) {
            want_framecount = true;
            break;
        }
    }
    if (want_framecount != plan_.framecount_osd_enabled) {
        plan_.framecount_osd_enabled = want_framecount;
        std::cout
            << "RetroArch Frames OSD "
            << (want_framecount ? "enabled" : "disabled")
            << " (client request)\n";
        if (!want_framecount) {
            plan_.framecount_osd_tick = 0;
        }
    }
    if (plan_.framecount_osd_enabled &&
        (plan_.framecount_osd_last_sent.time_since_epoch().count() == 0 ||
         now - plan_.framecount_osd_last_sent >= kFramecountOsdInterval)) {
        // RetroArch has no netcmd for framecount_show; SHOW_MSG is the live toggle path.
        // Changing text each tick also forces GL/Xvfb presents on static menus.
        const auto message = "Frames: " + std::to_string(plan_.framecount_osd_tick++);
        if (send_retroarch_netcmd(
                std::string("SHOW_MSG ") + message,
                plan_.retroarch_netcmd_port)) {
            plan_.framecount_osd_last_sent = now;
        }
    }

    return std::nullopt;
}

void SessionControlMonitor::handle_heartbeat(
    SessionClientConnection& client,
    const ViewerHeartbeat& heartbeat) {
    const auto now = std::chrono::steady_clock::now();
    client.last_seen = now;
    client.wanted_tier = heartbeat.wanted_tier;
    client.max_bitrate_kbps = heartbeat.max_bitrate_kbps;
    client.show_framecount = heartbeat.show_framecount;

    if (!client.hello.wants_video) {
        return;
    }

    if (heartbeat.wanted_tier != MediaQualityTier::Auto) {
        const auto resolved = select_video_tier(
            heartbeat.wanted_tier,
            client.applied_tier,
            client.max_bitrate_kbps);
        if (resolved != client.applied_tier) {
            apply_video_tier(client, resolved, "client requested tier");
        }
        client.bad_health_streak = 0;
        client.good_health_streak = 0;
        return;
    }

    // Wait for media to settle before using best-effort loss/frame stats for Auto.
    if (now - started_at_ < kStartupHeartbeatGrace) {
        return;
    }
    if (client.last_video_reconfigure.time_since_epoch().count() != 0 &&
        now - client.last_video_reconfigure < kPostReconfigureGrace) {
        return;
    }

    // Client Auto mode only reports health; the host decides ladder steps.
    // Enforce the Auto ceiling even if a prior build already climbed too high —
    // one demote restart is better than staying on 1080p60@25 Mbps with freezes.
    if (tier_above(client.applied_tier, kAutoMaxTier)) {
        apply_video_tier(client, kAutoMaxTier, "auto ceiling (cap High/Very-High)");
        return;
    }

    // Step down ONLY on real loss / dead receiver (client sets loss=1000‰ when
    // gst-launch is gone). Do NOT treat frames_decoded_delta==0 as unhealthy:
    // that telemetry is best-effort (Windows D3D11 zero-copy has no per-frame
    // probe; even progressreport is ~1 Hz line counts, not FPS). Missing or
    // coarse frame counts previously forced Auto → Low and never recovered.
    const bool hard_loss = heartbeat.loss_permille >= kHighLossPermille;
    if (hard_loss) {
        ++client.bad_health_streak;
        client.good_health_streak = 0;
        const auto bad_needed =
            (client.applied_tier == MediaQualityTier::High ||
             client.applied_tier == MediaQualityTier::MediumHigh ||
             client.applied_tier == MediaQualityTier::VeryHigh)
                ? kBadHealthThresholdFromHigh
                : kBadHealthThreshold;
        if (client.bad_health_streak >= bad_needed) {
            const auto previous = client.applied_tier;
            const auto next = step_quality_tier_down(client.applied_tier);
            if (next != client.applied_tier) {
                apply_video_tier(client, next, "auto step-down (loss)");
                if (previous == MediaQualityTier::High ||
                    previous == MediaQualityTier::VeryHigh ||
                    previous == MediaQualityTier::MediumHigh) {
                    client.high_tier_cooldown_until = now + kHighTierFailureCooldown;
                }
            }
            client.bad_health_streak = 0;
        }
        return;
    }

    client.bad_health_streak = 0;
    const auto next = step_quality_tier_up(client.applied_tier);
    if (next == client.applied_tier || tier_above(next, kAutoMaxTier)) {
        client.good_health_streak = 0;
        return;
    }

    const bool promoting_to_60fps =
        next == MediaQualityTier::MediumHigh ||
        next == MediaQualityTier::High ||
        next == MediaQualityTier::VeryHigh;
    const auto good_needed =
        promoting_to_60fps ? kGoodHealthThresholdForHigh : kGoodHealthThreshold;
    // True FPS probes are not available on all receive paths (gst-launch + D3D11
    // zero-copy). Promote Low→Medium on sustained no-loss heartbeats. Climbing
    // into 60 fps without frame telemetry is blind and previously walked Auto
    // into Very-High with encode restarts every ~45s (full-screen freezes).
    if (promoting_to_60fps && heartbeat.frames_decoded_delta == 0) {
        client.good_health_streak = 0;
        return;
    }
    if (heartbeat.frames_decoded_delta >= 2 &&
        heartbeat.frames_decoded_delta <
            (promoting_to_60fps ? kMinFramesForHighStepUp : kMinFramesForStepUp)) {
        client.good_health_streak = 0;
        return;
    }
    if (promoting_to_60fps &&
        client.high_tier_cooldown_until.time_since_epoch().count() != 0 &&
        now < client.high_tier_cooldown_until) {
        client.good_health_streak = 0;
        return;
    }

    ++client.good_health_streak;
    if (client.good_health_streak >= good_needed) {
        apply_video_tier(client, next, "auto step-up (healthy)");
        client.good_health_streak = 0;
    }
}

void SessionControlMonitor::apply_video_tier(
    SessionClientConnection& client,
    MediaQualityTier tier,
    std::string_view reason) {
    const auto now = std::chrono::steady_clock::now();
    if (client.last_video_reconfigure.time_since_epoch().count() != 0 &&
        now - client.last_video_reconfigure < kMinReconfigureInterval) {
        return;
    }

    // Selector: map wanted/auto step onto a ladder branch, capped by client max bitrate.
    const auto resolved = select_video_tier(tier, client.applied_tier, client.max_bitrate_kbps);
    const auto settings = video_encode_settings_for_tier(resolved);
    if (!media_server_.reconfigure_client_video(client.client_id, settings)) {
        return;
    }

    client.applied_tier = resolved;
    client.last_video_reconfigure = now;
    client.bad_health_streak = 0;
    client.good_health_streak = 0;
    // Hint the client to one-shot resync A/V (same URIs; video encoder restarted).
    if (client.media_endpoint.has_value() &&
        client.connection_state == SessionConnectionState::Connected) {
        try {
            client.stream.send_packet(serialize_packet(*client.media_endpoint));
        } catch (const std::exception&) {
        }
    }
    std::cerr
        << "Adapted video for " << client_label(client)
        << " -> " << media_quality_tier_name(resolved)
        << " (" << settings.bitrate_kbps << " kbps, "
        << static_cast<int>(settings.framerate) << " fps";
    if (settings.width > 0 && settings.height > 0) {
        std::cerr << ", " << settings.width << "x" << settings.height;
    }
    std::cerr << "): " << reason << '\n';
}

bool SessionControlMonitor::remove_viewer(std::size_t index, std::string_view reason) {
    if (plan_.clients[index].hello.requested_players != 0) {
        return false;
    }

    std::cerr
        << "Removing viewer " << static_cast<int>(plan_.clients[index].client_id)
        << " (" << plan_.clients[index].hello.username << "): "
        << reason << '\n';
    plan_.link_coordinator.clear_client(plan_.clients[index].client_id);
    if (host_hub_ != nullptr) {
        host_hub_->clear_link_client(plan_.clients[index].client_id);
    }
    if (plan_.clients[index].client_id == plan_.link_cable.client_a() ||
        plan_.clients[index].client_id == plan_.link_cable.client_b()) {
        plan_.link_cable.clear();
    }
    media_server_.remove_client(plan_.clients[index].client_id);
    plan_.clients.erase(plan_.clients.begin() + static_cast<std::ptrdiff_t>(index));
    return true;
}

void SessionControlMonitor::mark_player_disconnected(SessionClientConnection& client, std::string_view reason) {
    plan_.link_coordinator.clear_client(client.client_id);
    if (host_hub_ != nullptr) {
        host_hub_->clear_link_client(client.client_id);
    }
    if (client.client_id == plan_.link_cable.client_a() ||
        client.client_id == plan_.link_cable.client_b()) {
        plan_.link_cable.clear();
    }
    media_server_.remove_client(client.client_id);
    client.connection_state = SessionConnectionState::Disconnected;
    client.disconnected_at = std::chrono::steady_clock::now();
    client.disconnect_reason = std::string(reason);
    input_router_.neutralize_client(client.client_id);
    const auto grace = reconnect_grace_for(client, reconnect_timeout_);
    std::cerr
        << "Player " << static_cast<int>(client.client_id)
        << " (" << client.hello.username << ") disconnected: "
        << reason;
    if (grace.count() == 0) {
        std::cerr << "; ending seat immediately (client left)\n";
    } else {
        std::cerr << "; reserving seats for " << grace.count() << "s\n";
    }
}

std::string SessionControlMonitor::client_label(const SessionClientConnection& client) {
    std::ostringstream out;
    out << "client " << static_cast<int>(client.client_id) << " (" << client.hello.username << ")";
    return out.str();
}

} // namespace archstreamer
