#include "host/session_control_monitor.hpp"

#include "common/client_logs.hpp"
#include "common/serialization.hpp"
#include "host/active_session_slot.hpp"
#include "host/cadence_session_events.hpp"
#include "host/controls_db_sync.hpp"
#include "host/host_session_hub.hpp"
#include "host/retroarch_config_writer.hpp"
#include "host/retroarch_netcmd.hpp"
#include "host/save_active_sessions.hpp"
#include "host/user_credentials.hpp"

#include <algorithm>
#include <iostream>
#include <sstream>
#include <variant>

namespace archstreamer {
namespace {

constexpr auto kStartupHeartbeatGrace = std::chrono::seconds(15);
constexpr auto kMinReconfigureInterval = std::chrono::seconds(5);
// After cutover promote the remote may briefly report 0 decoded frames until IDR.
constexpr auto kPostReconfigureGrace = std::chrono::seconds(12);
// The client only ACKs after proving the staging port really carries video, so
// this has to cover probe startup plus a couple of seconds of confirmed frames.
constexpr auto kVideoCutoverTimeout = std::chrono::seconds(12);
// After a failed High stay, wait before retrying — High is heavier on Wi‑Fi.
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
// Auto must not walk into High/Very-High: each step still costs a dual-stream
// cutover. 1080p60@12–25 Mbps also overloads many Wi‑Fi clients. Players can
// still pick High/Very-High explicitly in the client UI.
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

void sync_applied_to_session(SessionClientConnection& client, const SessionPlan& plan) {
    client.applied_size = plan.session_video_size;
    client.applied_tier = plan.session_video_tier;
    client.applied_feel = plan.session_video_feel;
    client.applied_bitrate = plan.session_video_bitrate;
}

void sync_all_applied_to_session(SessionPlan& plan) {
    for (auto& client : plan.clients) {
        if (client.connection_state != SessionConnectionState::Connected) {
            continue;
        }
        sync_applied_to_session(client, plan);
    }
}

int stream_feel_rank(MediaStreamFeel feel) {
    switch (feel) {
    case MediaStreamFeel::Smooth:
        return 2;
    case MediaStreamFeel::Balanced:
        return 1;
    case MediaStreamFeel::LowLatency:
    default:
        return 0;
    }
}

struct PlayerEncodeContribution {
    VideoEncodeSettings settings{};
    MediaStreamSize size = MediaStreamSize::P720;
    MediaQualityTier tier = MediaQualityTier::Medium;
    MediaStreamFeel feel = MediaStreamFeel::LowLatency;
    MediaStreamBitrate bitrate = MediaStreamBitrate::Auto;
};

PlayerEncodeContribution player_encode_contribution(
    const SessionClientConnection& client,
    std::uint16_t capture_width,
    std::uint16_t capture_height,
    MediaStreamSize size_override,
    MediaQualityTier tier_override,
    MediaStreamFeel feel_override,
    MediaStreamBitrate bitrate_override,
    bool use_override) {
    MediaStreamSize size = use_override ? size_override : client.wanted_size;
    if (size == MediaStreamSize::Auto) {
        size = client.applied_size;
    }
    const MediaQualityTier wanted =
        use_override ? tier_override : client.wanted_tier;
    const auto tier = select_video_tier(wanted, client.applied_tier, client.max_bitrate_kbps);
    const MediaStreamFeel feel = use_override ? feel_override : client.wanted_feel;
    const MediaStreamBitrate bitrate =
        use_override ? bitrate_override : client.wanted_bitrate;
    return PlayerEncodeContribution{
        video_encode_settings(size, tier, capture_width, capture_height, feel, bitrate),
        size,
        tier,
        feel,
        bitrate,
    };
}

struct SessionVideoCeiling {
    VideoEncodeSettings settings{};
    MediaStreamSize size = MediaStreamSize::P720;
    MediaQualityTier tier = MediaQualityTier::Medium;
    MediaStreamFeel feel = MediaStreamFeel::LowLatency;
    MediaStreamBitrate bitrate = MediaStreamBitrate::Auto;
    bool any_player = false;
};

SessionVideoCeiling compute_session_video_ceiling(
    const SessionPlan& plan,
    std::uint16_t capture_width,
    std::uint16_t capture_height,
    ClientId override_client_id,
    MediaStreamSize size_override,
    MediaQualityTier tier_override,
    MediaStreamFeel feel_override,
    MediaStreamBitrate bitrate_override,
    bool use_override_client) {
    SessionVideoCeiling ceiling{};
    bool all_bitrate_auto = true;
    for (const auto& client : plan.clients) {
        if (!client_is_seated_player(client) ||
            client.connection_state != SessionConnectionState::Connected ||
            !client.hello.wants_video) {
            continue;
        }
        const bool use_override =
            use_override_client && client.client_id == override_client_id;
        const auto contrib = player_encode_contribution(
            client,
            capture_width,
            capture_height,
            size_override,
            tier_override,
            feel_override,
            bitrate_override,
            use_override);
        if (contrib.bitrate != MediaStreamBitrate::Auto) {
            all_bitrate_auto = false;
        }
        if (!ceiling.any_player) {
            ceiling.settings = contrib.settings;
            ceiling.size = contrib.size;
            ceiling.tier = contrib.tier;
            ceiling.feel = contrib.feel;
            ceiling.bitrate = contrib.bitrate;
            ceiling.any_player = true;
            continue;
        }
        ceiling.settings = dominate_video_encode_settings(ceiling.settings, contrib.settings);
        if (media_stream_size_height(contrib.size) > media_stream_size_height(ceiling.size)) {
            ceiling.size = contrib.size;
        }
        if (tier_above(contrib.tier, ceiling.tier)) {
            ceiling.tier = contrib.tier;
        }
        if (stream_feel_rank(contrib.feel) > stream_feel_rank(ceiling.feel)) {
            ceiling.feel = contrib.feel;
        }
    }
    if (!ceiling.any_player) {
        if (plan.session_video_configured) {
            ceiling.settings = plan.session_video_settings;
            ceiling.size = plan.session_video_size;
            ceiling.tier = plan.session_video_tier;
            ceiling.feel = plan.session_video_feel;
            ceiling.bitrate = plan.session_video_bitrate;
        } else {
            ceiling.settings = video_encode_settings(
                MediaStreamSize::P720,
                MediaQualityTier::Medium,
                capture_width,
                capture_height);
            ceiling.size = MediaStreamSize::P720;
            ceiling.tier = MediaQualityTier::Medium;
            ceiling.feel = MediaStreamFeel::LowLatency;
            ceiling.bitrate = MediaStreamBitrate::Auto;
        }
        return ceiling;
    }
    ceiling.bitrate = all_bitrate_auto
        ? MediaStreamBitrate::Auto
        : media_stream_bitrate_for_settings(ceiling.settings);
    return ceiling;
}

std::chrono::seconds reconnect_grace_for(const SessionClientConnection& client, std::chrono::seconds full) {
    // Explicit ClientSessionLeave / admin Kick → end immediately (no reconnect hold).
    // TCP close / heartbeat loss → full reconnect window for flaky links.
    if (client.disconnect_reason == "left" || client.disconnect_reason == "kicked") {
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
    HostSessionHub* host_hub,
    std::uint16_t capture_width,
    std::uint16_t capture_height,
    std::filesystem::path save_root,
    int slot_index,
    std::string session_id)
    : plan_(plan),
      input_router_(input_router),
      media_server_(media_server),
      heartbeat_timeout_(heartbeat_timeout),
      reconnect_timeout_(reconnect_timeout),
      started_at_(std::chrono::steady_clock::now()),
      host_hub_(host_hub),
      capture_width_(capture_width == 0 ? 1920 : capture_width),
      capture_height_(capture_height == 0 ? 1080 : capture_height),
      save_root_(std::move(save_root)),
      slot_index_(slot_index),
      session_id_(std::move(session_id)) {
    const auto now = started_at_;
    // Shared encode starts at Medium@720p; seated players may raise the session ceiling.
    plan_.session_video_settings = video_encode_settings(
        MediaStreamSize::P720,
        MediaQualityTier::Medium,
        capture_width_,
        capture_height_);
    plan_.session_video_size = MediaStreamSize::P720;
    plan_.session_video_tier = MediaQualityTier::Medium;
    plan_.session_video_feel = MediaStreamFeel::LowLatency;
    plan_.session_video_bitrate = MediaStreamBitrate::Auto;
    plan_.session_video_configured = true;
    for (auto& client : plan_.clients) {
        client.last_seen = now;
        client.wanted_tier = MediaQualityTier::Auto;
        client.wanted_size = MediaStreamSize::Auto;
        client.wanted_feel = MediaStreamFeel::LowLatency;
        client.wanted_bitrate = MediaStreamBitrate::Auto;
        sync_applied_to_session(client, plan_);
    }
}

std::optional<std::string> SessionControlMonitor::poll() {
    const auto now = std::chrono::steady_clock::now();
    const auto in_startup_grace = now - started_at_ < kStartupHeartbeatGrace;

    // Users-tab Kick of a single connection (viewer or seated player).
    for (std::size_t i = 0; i < plan_.clients.size();) {
        auto& client = plan_.clients[i];
        if (client.connection_state != SessionConnectionState::Connected) {
            ++i;
            continue;
        }
        auto reason = take_connected_client_disconnect_request(
            save_root_, client.client_id, slot_index_);
        if (!reason.has_value()) {
            ++i;
            continue;
        }
        std::cerr
            << "Admin disconnect client " << static_cast<int>(client.client_id)
            << " (" << client.hello.username << "): " << *reason << '\n';
        try {
            client.stream = TcpStream{};
        } catch (const std::exception&) {
        }
        clear_connected_client(save_root_, client.client_id, slot_index_);
        if (remove_viewer(i, *reason)) {
            continue;
        }
        // Treat admin kick like an explicit leave (no reconnect grace).
        mark_player_disconnected(client, "left");
        client.disconnect_reason = *reason;
        if (!any_connected_seated_player(plan_)) {
            return client_label(client) + " kicked; ending session for a new lobby";
        }
        if (plan_.session_mode == GameSessionMode::SinglePlayer) {
            return client_label(client) + " kicked; ending singleplayer session";
        }
        ++i;
    }

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
                std::cout
                    << "ClientSessionLeave from " << client_label(client)
                    << " reason=\"" << reason << "\"\n";
                if (remove_viewer(i, reason)) {
                    removed_current = true;
                    break;
                }
                mark_player_disconnected(client, "left");
                if (!any_connected_seated_player(plan_)) {
                    return client_label(client) + " left; ending session for a new lobby";
                }
                // Singleplayer: one seated player left — end even if viewers remain wait.
                if (plan_.session_mode == GameSessionMode::SinglePlayer) {
                    return client_label(client) + " left; ending singleplayer session";
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
                    if (soft_keyboard->request_id == 0) {
                        std::cout
                            << "Soft keyboard manual inject"
                            << " accepted=" << (soft_keyboard->accepted ? "yes" : "no")
                            << " from " << client_label(client) << '\n';
                    } else {
                        std::cout
                            << "Soft keyboard response id=" << soft_keyboard->request_id
                            << " accepted=" << (soft_keyboard->accepted ? "yes" : "no")
                            << " from " << client_label(client) << '\n';
                    }
                }
            } else if (const auto* emu_control = std::get_if<EmulatorControl>(&payload);
                       emu_control != nullptr) {
                if (emu_control->client_id == client.client_id) {
                    input_router_.apply_emulator_control(*emu_control);
                }
            } else if (const auto* log_bundle = std::get_if<ClientLogBundle>(&payload);
                       log_bundle != nullptr) {
                try {
                    client.stream.send_packet(
                        serialize_packet(acknowledge_client_log_bundle(*log_bundle)));
                } catch (const std::exception&) {
                }
            } else if (const auto* password_change = std::get_if<PasswordChange>(&payload);
                       password_change != nullptr) {
                try {
                    client.stream.send_packet(serialize_packet(
                        acknowledge_password_change(save_root_, *password_change)));
                } catch (const std::exception&) {
                }
            } else if (std::holds_alternative<ControlsDbPull>(payload)
                       || std::holds_alternative<ControlsDbPush>(payload)) {
                try {
                    const auto claimed = !client.hello.username.empty()
                        ? client.hello.username
                        : plan_.save_username;
                    auto reply = handle_controls_db_packet(save_root_, claimed, payload);
                    if (!reply.empty()) {
                        client.stream.send_packet(reply);
                    }
                } catch (const std::exception&) {
                }
            } else if (const auto* video_ready = std::get_if<MediaVideoReady>(&payload);
                       video_ready != nullptr) {
                // Legacy dual-stream ACK. Quality changes now hard-restart the shared
                // encode; clear any stale pending state without promoting a dedicated path.
                if (client.pending_video_uri.has_value()) {
                    media_server_.abort_video_tier_cutover(client.client_id);
                    client.pending_video_uri.reset();
                    client.pending_tier.reset();
                    client.pending_size.reset();
                    client.pending_feel.reset();
                    client.pending_bitrate.reset();
                    client.video_cutover_started = {};
                    std::cerr
                        << "Ignoring legacy MediaVideoReady from " << client_label(client)
                        << " (shared encode has no staging cutover)\n";
                }
                (void)video_ready;
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

        if (client.pending_video_uri.has_value() &&
            client.video_cutover_started.time_since_epoch().count() != 0 &&
            now - client.video_cutover_started >= kVideoCutoverTimeout) {
            media_server_.abort_video_tier_cutover(client.client_id);
            std::cerr
                << "Clearing stale video pending for " << client_label(client)
                << " (shared encode no longer stages)\n";
            client.pending_video_uri.reset();
            client.pending_tier.reset();
            client.pending_size.reset();
            client.pending_feel.reset();
            client.pending_bitrate.reset();
            client.video_cutover_started = {};
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
    client.wanted_size = heartbeat.wanted_size;
    client.wanted_feel = heartbeat.wanted_feel;
    client.wanted_bitrate = heartbeat.wanted_bitrate;
    client.max_bitrate_kbps = heartbeat.max_bitrate_kbps;
    client.show_framecount = heartbeat.show_framecount;

    if (heartbeat.display_layout != DisplayLayoutPreference::Auto &&
        heartbeat.display_layout != client.display_layout) {
        client.display_layout = heartbeat.display_layout;
        client.hello.display_layout = heartbeat.display_layout;
        if (plan_.system_key == "nds") {
            apply_nds_screen_layout(heartbeat.display_layout);
        }
    } else if (heartbeat.display_layout != DisplayLayoutPreference::Auto) {
        client.display_layout = heartbeat.display_layout;
        client.hello.display_layout = heartbeat.display_layout;
    }

    if (!client.hello.wants_video) {
        return;
    }

    // Viewers only receive the session encode; never raise or Auto-ladder it.
    if (!client_is_seated_player(client)) {
        sync_applied_to_session(client, plan_);
        client.bad_health_streak = 0;
        client.good_health_streak = 0;
        return;
    }

    if (client.pending_video_uri.has_value() ||
        media_server_.video_cutover_in_flight(client.client_id)) {
        return;
    }

    // Client resolves Auto size before send; omitted/legacy → keep applied size.
    const MediaStreamSize resolved_size =
        heartbeat.wanted_size == MediaStreamSize::Auto
            ? client.applied_size
            : heartbeat.wanted_size;
    const MediaStreamFeel resolved_feel = heartbeat.wanted_feel;
    const MediaStreamBitrate resolved_bitrate = heartbeat.wanted_bitrate;
    const bool bitrate_fixed = resolved_bitrate != MediaStreamBitrate::Auto;

    auto restage_reason = [&](
        MediaQualityTier tier,
        MediaStreamSize size,
        MediaStreamFeel feel,
        MediaStreamBitrate bitrate) -> const char* {
        const bool tier_changed = tier != client.applied_tier;
        const bool size_changed = size != client.applied_size;
        const bool feel_changed = feel != client.applied_feel;
        const bool bitrate_changed = bitrate != client.applied_bitrate;
        if (feel_changed && !tier_changed && !size_changed && !bitrate_changed) {
            return "client requested stream feel";
        }
        if (bitrate_changed && !tier_changed && !size_changed && !feel_changed) {
            return "client requested bitrate";
        }
        if (size_changed && !tier_changed && !feel_changed && !bitrate_changed) {
            return "client requested size";
        }
        if (tier_changed && !size_changed && !feel_changed && !bitrate_changed) {
            return "client requested frame rate";
        }
        return "client requested size/quality";
    };

    if (heartbeat.wanted_tier != MediaQualityTier::Auto) {
        const auto resolved = select_video_tier(
            heartbeat.wanted_tier,
            client.applied_tier,
            client.max_bitrate_kbps);
        if (resolved != client.applied_tier ||
            resolved_size != client.applied_size ||
            resolved_feel != client.applied_feel ||
            resolved_bitrate != client.applied_bitrate) {
            apply_video_encode(
                client,
                resolved_size,
                resolved,
                resolved_feel,
                resolved_bitrate,
                restage_reason(resolved, resolved_size, resolved_feel, resolved_bitrate));
        }
        client.bad_health_streak = 0;
        client.good_health_streak = 0;
        return;
    }

    // Size / feel / bitrate can still change under Auto frame rate.
    if (resolved_size != client.applied_size ||
        resolved_feel != client.applied_feel ||
        resolved_bitrate != client.applied_bitrate) {
        apply_video_encode(
            client,
            resolved_size,
            client.applied_tier,
            resolved_feel,
            resolved_bitrate,
            restage_reason(
                client.applied_tier, resolved_size, resolved_feel, resolved_bitrate));
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

    // Auto ceiling: with fixed bitrate only climb Low/Medium/High; with Auto bitrate
    // keep the legacy combined ladder ceiling (MediumHigh).
    const MediaQualityTier auto_ceiling =
        bitrate_fixed ? MediaQualityTier::High : kAutoMaxTier;
    if (tier_above(client.applied_tier, auto_ceiling)) {
        apply_video_encode(
            client,
            client.applied_size,
            auto_ceiling,
            client.applied_feel,
            client.applied_bitrate,
            bitrate_fixed ? "auto ceiling (frame rate)" : "auto ceiling (cap High/Very-High)");
        return;
    }

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
            const auto next = bitrate_fixed
                ? step_framerate_tier_down(client.applied_tier)
                : step_quality_tier_down(client.applied_tier);
            if (next != client.applied_tier) {
                apply_video_encode(
                    client,
                    client.applied_size,
                    next,
                    client.applied_feel,
                    client.applied_bitrate,
                    "auto step-down (loss)");
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
    const auto next = bitrate_fixed
        ? step_framerate_tier_up(client.applied_tier)
        : step_quality_tier_up(client.applied_tier);
    if (next == client.applied_tier || tier_above(next, auto_ceiling)) {
        client.good_health_streak = 0;
        return;
    }

    const bool promoting_to_60fps =
        next == MediaQualityTier::MediumHigh ||
        next == MediaQualityTier::High ||
        next == MediaQualityTier::VeryHigh;
    const auto good_needed =
        promoting_to_60fps ? kGoodHealthThresholdForHigh : kGoodHealthThreshold;
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
        apply_video_encode(
            client,
            client.applied_size,
            next,
            client.applied_feel,
            client.applied_bitrate,
            "auto step-up (healthy)");
        client.good_health_streak = 0;
    }
}

void SessionControlMonitor::apply_video_encode(
    SessionClientConnection& client,
    MediaStreamSize size,
    MediaQualityTier tier,
    MediaStreamFeel feel,
    MediaStreamBitrate bitrate,
    std::string_view reason) {
    if (!client_is_seated_player(client)) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (client.video_cutover_suppressed) {
        return;
    }
    if (client.pending_video_uri.has_value() ||
        media_server_.video_cutover_in_flight(client.client_id)) {
        return;
    }
    if (client.last_video_reconfigure.time_since_epoch().count() != 0 &&
        now - client.last_video_reconfigure < kMinReconfigureInterval) {
        return;
    }

    if (size == MediaStreamSize::Auto) {
        size = client.applied_size;
    }
    const auto resolved = select_video_tier(tier, client.applied_tier, client.max_bitrate_kbps);
    const auto ceiling = compute_session_video_ceiling(
        plan_,
        capture_width_,
        capture_height_,
        client.client_id,
        size,
        resolved,
        feel,
        bitrate,
        true);

    client.bad_health_streak = 0;
    client.good_health_streak = 0;

    if (plan_.session_video_configured &&
        ceiling.settings == plan_.session_video_settings) {
        // Contribution changed but session encode did not; mirror what this client receives.
        sync_applied_to_session(client, plan_);
        return;
    }

    if (!media_server_.reconfigure_shared_video(ceiling.settings)) {
        return;
    }

    plan_.session_video_settings = ceiling.settings;
    plan_.session_video_size = ceiling.size;
    plan_.session_video_tier = ceiling.tier;
    plan_.session_video_feel = ceiling.feel;
    plan_.session_video_bitrate = ceiling.bitrate;
    plan_.session_video_configured = true;
    sync_all_applied_to_session(plan_);
    for (auto& other : plan_.clients) {
        if (other.connection_state == SessionConnectionState::Connected) {
            other.last_video_reconfigure = now;
        }
    }

    std::cerr
        << "Session video -> " << media_stream_size_name(ceiling.size)
        << "/" << media_quality_tier_name(ceiling.tier)
        << "/" << media_stream_bitrate_name(ceiling.bitrate)
        << "/" << media_stream_feel_name(ceiling.feel)
        << " (" << ceiling.settings.bitrate_kbps << " kbps, "
        << static_cast<int>(ceiling.settings.framerate) << " fps";
    if (ceiling.settings.width > 0 && ceiling.settings.height > 0) {
        std::cerr << ", " << ceiling.settings.width << "x" << ceiling.settings.height;
    }
    std::cerr
        << ", queue=" << static_cast<int>(ceiling.settings.queue_buffers)
        << ", nvenc=" << (ceiling.settings.nvenc_high_quality ? "hq" : "hp")
        << ") from " << client_label(client)
        << ": " << reason << '\n';
}

bool SessionControlMonitor::remove_viewer(std::size_t index, std::string_view reason) {
    if (plan_.clients[index].hello.requested_players != 0) {
        return false;
    }

    const auto username = plan_.clients[index].hello.username;
    std::cerr
        << "Removing viewer " << static_cast<int>(plan_.clients[index].client_id)
        << " (" << username << "): "
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
    clear_connected_client(save_root_, plan_.clients[index].client_id, slot_index_);
    plan_.clients.erase(plan_.clients.begin() + static_cast<std::ptrdiff_t>(index));
    record_client_left(
        slot_index_,
        username,
        plan_.selected_game_id,
        std::string(reason),
        session_id_);
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
    clear_connected_client(save_root_, client.client_id, slot_index_);
    client.connection_state = SessionConnectionState::Disconnected;
    client.disconnected_at = std::chrono::steady_clock::now();
    client.disconnect_reason = std::string(reason);
    client.pending_tier.reset();
    client.pending_size.reset();
    client.pending_feel.reset();
    client.pending_bitrate.reset();
    client.pending_video_uri.reset();
    client.video_cutover_started = {};
    input_router_.neutralize_client(client.client_id);

    // Drop this seat's contribution; remaining players own the ceiling.
    if (plan_.session_video_configured && client.hello.wants_video) {
        const auto ceiling = compute_session_video_ceiling(
            plan_,
            capture_width_,
            capture_height_,
            client.client_id,
            MediaStreamSize::P720,
            MediaQualityTier::Medium,
            MediaStreamFeel::LowLatency,
            MediaStreamBitrate::Auto,
            false);
        if (ceiling.settings != plan_.session_video_settings) {
            if (media_server_.reconfigure_shared_video(ceiling.settings)) {
                plan_.session_video_settings = ceiling.settings;
                plan_.session_video_size = ceiling.size;
                plan_.session_video_tier = ceiling.tier;
                plan_.session_video_feel = ceiling.feel;
                plan_.session_video_bitrate = ceiling.bitrate;
                sync_all_applied_to_session(plan_);
                const auto now = std::chrono::steady_clock::now();
                for (auto& other : plan_.clients) {
                    if (other.connection_state == SessionConnectionState::Connected) {
                        other.last_video_reconfigure = now;
                    }
                }
                std::cerr
                    << "Session video -> " << media_stream_size_name(ceiling.size)
                    << "/" << media_quality_tier_name(ceiling.tier)
                    << " after " << client_label(client) << " left\n";
            }
        }
    }

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
    record_client_left(
        slot_index_,
        client.hello.username,
        plan_.selected_game_id,
        std::string(reason),
        session_id_);
}

std::string SessionControlMonitor::client_label(const SessionClientConnection& client) {
    std::ostringstream out;
    out << "client " << static_cast<int>(client.client_id) << " (" << client.hello.username << ")";
    return out.str();
}

} // namespace archstreamer
