#include "host/session_control_monitor.hpp"

#include "common/serialization.hpp"

#include <algorithm>
#include <iostream>
#include <sstream>
#include <variant>

namespace archstreamer {
namespace {

constexpr auto kStartupHeartbeatGrace = std::chrono::seconds(15);
constexpr auto kMinReconfigureInterval = std::chrono::seconds(5);
// Clean leave (closed window / Stop Client): don't hold the whole host for a full minute.
constexpr auto kCleanDisconnectGrace = std::chrono::seconds(15);
constexpr std::uint8_t kBadHealthThreshold = 3;
constexpr std::uint8_t kGoodHealthThreshold = 10;
constexpr std::uint16_t kHighLossPermille = 100;

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
    // Intentional leave → short grace so the host can return to lobby for a new game.
    // Heartbeat loss → keep the full reconnect window for flaky links.
    if (client.disconnect_reason == "disconnected") {
        return std::min(full, kCleanDisconnectGrace);
    }
    return full;
}

} // namespace

SessionControlMonitor::SessionControlMonitor(
    SessionPlan& plan,
    InputRouter& input_router,
    MediaServer& media_server,
    std::chrono::seconds heartbeat_timeout,
    std::chrono::seconds reconnect_timeout)
    : plan_(plan),
      input_router_(input_router),
      media_server_(media_server),
      heartbeat_timeout_(heartbeat_timeout),
      reconnect_timeout_(reconnect_timeout),
      started_at_(std::chrono::steady_clock::now()) {
    const auto now = started_at_;
    for (auto& client : plan_.clients) {
        client.last_seen = now;
        // Start at Medium: High is 12 Mbps/60 and can overwhelm Wi‑Fi remotes before
        // the first heartbeat. Auto can still step up when the link is healthy.
        client.applied_tier = MediaQualityTier::Medium;
        client.wanted_tier = MediaQualityTier::Auto;
    }
}

std::optional<std::string> SessionControlMonitor::poll() {
    const auto now = std::chrono::steady_clock::now();
    const auto in_startup_grace = now - started_at_ < kStartupHeartbeatGrace;
    for (std::size_t i = 0; i < plan_.clients.size();) {
        auto& client = plan_.clients[i];
        if (client.connection_state == SessionConnectionState::Disconnected) {
            const auto grace = reconnect_grace_for(client, reconnect_timeout_);
            if (now - client.disconnected_at > grace) {
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

    return std::nullopt;
}

void SessionControlMonitor::handle_heartbeat(
    SessionClientConnection& client,
    const ViewerHeartbeat& heartbeat) {
    const auto now = std::chrono::steady_clock::now();
    client.last_seen = now;
    client.wanted_tier = heartbeat.wanted_tier;
    client.max_bitrate_kbps = heartbeat.max_bitrate_kbps;

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

    const bool unhealthy =
        heartbeat.loss_permille >= kHighLossPermille || heartbeat.frames_decoded_delta == 0;
    if (unhealthy) {
        ++client.bad_health_streak;
        client.good_health_streak = 0;
        if (client.bad_health_streak >= kBadHealthThreshold) {
            const auto next = step_quality_tier_down(client.applied_tier);
            if (next != client.applied_tier) {
                apply_video_tier(client, next, "auto step-down (loss/no frames)");
            }
            client.bad_health_streak = 0;
        }
        return;
    }

    ++client.good_health_streak;
    client.bad_health_streak = 0;
    if (client.good_health_streak >= kGoodHealthThreshold) {
        const auto next = step_quality_tier_up(client.applied_tier);
        if (next != client.applied_tier) {
            apply_video_tier(client, next, "auto step-up (healthy)");
        }
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
    std::cerr
        << "Adapted video for " << client_label(client)
        << " -> " << media_quality_tier_name(resolved)
        << " (" << settings.bitrate_kbps << " kbps, "
        << static_cast<int>(settings.framerate) << " fps): "
        << reason << '\n';
}

bool SessionControlMonitor::remove_viewer(std::size_t index, std::string_view reason) {
    if (plan_.clients[index].hello.requested_players != 0) {
        return false;
    }

    std::cerr
        << "Removing viewer " << static_cast<int>(plan_.clients[index].client_id)
        << " (" << plan_.clients[index].hello.username << "): "
        << reason << '\n';
    media_server_.remove_client(plan_.clients[index].client_id);
    plan_.clients.erase(plan_.clients.begin() + static_cast<std::ptrdiff_t>(index));
    return true;
}

void SessionControlMonitor::mark_player_disconnected(SessionClientConnection& client, std::string_view reason) {
    media_server_.remove_client(client.client_id);
    client.connection_state = SessionConnectionState::Disconnected;
    client.disconnected_at = std::chrono::steady_clock::now();
    client.disconnect_reason = std::string(reason);
    input_router_.neutralize_client(client.client_id);
    const auto grace = reconnect_grace_for(client, reconnect_timeout_);
    std::cerr
        << "Player " << static_cast<int>(client.client_id)
        << " (" << client.hello.username << ") disconnected: "
        << reason << "; reserving seats for "
        << grace.count() << "s\n";
}

std::string SessionControlMonitor::client_label(const SessionClientConnection& client) {
    std::ostringstream out;
    out << "client " << static_cast<int>(client.client_id) << " (" << client.hello.username << ")";
    return out.str();
}

} // namespace archstreamer
