#include "host/session_control_monitor.hpp"

#include "common/client_logs.hpp"
#include "common/serialization.hpp"
#include "host/active_session_slot.hpp"
#include "host/cadence_session_events.hpp"
#include "host/client_stream_policy.hpp"
#include "host/controls_db_sync.hpp"
#include "host/host_session_hub.hpp"
#include "host/pair_form_relay.hpp"
#include "host/retroarch_config_writer.hpp"
#include "host/retroarch_netcmd.hpp"
#include "host/save_active_sessions.hpp"
#include "host/stream_adaptation.hpp"
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
// Hard restart is a last-resort fallback. Brief Chromecast decoder blanks often
// report frames=0 while RTP is still flowing (low loss) — do not thrash shared
// media for those. High loss means the pipe looks dead; allow a faster recovery.
constexpr std::uint8_t kDeadPipeHighLossStallThreshold = 8;
constexpr std::uint8_t kDeadPipeLowLossStallThreshold = 25;
constexpr std::uint8_t kTvInitialVideoReadyHeartbeats = 2;
// After promote/reconfigure, zero frames are expected until the new IDR lands.
constexpr auto kPostReconfigureStallGrace = std::chrono::seconds(12);
// Only watch for a never-recovered cutover — not two minutes of idle hiccups.
constexpr auto kPostReconfigureStallWindow = std::chrono::seconds(45);
constexpr auto kVideoStallRestartInterval = std::chrono::seconds(30);
constexpr auto kDecodePressureLogInterval = std::chrono::seconds(5);
constexpr std::uint16_t kTvDecodePressureP95Ms = 200;
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

bool any_connected_seated_player(const SessionPlan& plan) {
    for (const auto& client : plan.clients) {
        if (client_is_seated_player(client) &&
            client.connection_state == SessionConnectionState::Connected) {
            return true;
        }
    }
    return false;
}

VideoEncodeSettings video_encode_settings_for_client(
    const SessionClientConnection& client,
    MediaStreamSize size,
    MediaQualityTier tier,
    std::uint16_t capture_width,
    std::uint16_t capture_height,
    MediaStreamFeel feel,
    MediaStreamBitrate bitrate,
    MediaStreamFps fps) {
    auto settings = video_encode_settings(size, tier, capture_width, capture_height, feel, bitrate, fps);
    return apply_client_stream_policy(settings, client_stream_policy_for(client));
}

std::string stream_request_log_string(const StreamRequest& request) {
    std::ostringstream out;
    out << media_stream_size_name(request.size)
        << "/" << media_quality_tier_name(request.tier)
        << "/" << media_stream_bitrate_name(request.bitrate)
        << "/" << media_stream_feel_name(request.feel)
        << "/" << media_stream_fps_name(request.fps);
    return out.str();
}

void log_stream_request_resolution(
    const SessionClientConnection& client,
    std::string_view label,
    const StreamRequest& requested,
    const StreamRequestResolution& resolution) {
    if (!resolution.limited_by_metrics ||
        resolution.reason.empty() ||
        resolution.reason.find("hold current") != std::string_view::npos) {
        return;
    }
    std::cerr
        << "Stream request resolved for " << label
        << " device=" << client_device_class_name(client.hello.device.device_class)
        << " perf=" << client_performance_class_name(client.hello.device.performance_class)
        << " requested=" << stream_request_log_string(requested)
        << " effective=" << stream_request_log_string(resolution.request)
        << ": " << resolution.reason << '\n';
}

void reset_video_stall_tracking(SessionPlan& plan) {
    for (auto& client : plan.clients) {
        client.video_zero_frame_streak = 0;
    }
}

void sync_applied_to_session(SessionClientConnection& client, const SessionPlan& plan) {
    client.applied_size = plan.stream.video_size;
    client.applied_tier = plan.stream.video_tier;
    client.applied_feel = plan.stream.video_feel;
    client.applied_bitrate = plan.stream.video_bitrate;
    client.applied_fps = plan.stream.video_fps;
}

void sync_all_applied_to_session(SessionPlan& plan) {
    for (auto& client : plan.clients) {
        if (client.connection_state != SessionConnectionState::Connected) {
            continue;
        }
        sync_applied_to_session(client, plan);
    }
}

void sync_applied_from_branch_decision(
    SessionClientConnection& client,
    const StreamBranchDecision& decision,
    const SessionPlan& plan) {
    if (decision.action == StreamBranchAction::TakeTrunk) {
        sync_applied_to_session(client, plan);
        return;
    }
    client.applied_size = media_stream_size_for_settings(decision.settings);
    client.applied_tier = media_quality_tier_for_settings(decision.settings);
    client.applied_feel = media_stream_feel_for_settings(decision.settings);
    client.applied_bitrate = media_stream_bitrate_for_settings(decision.settings);
    client.applied_fps = media_stream_fps_for_framerate(decision.settings.framerate);
}

void sync_all_applied_from_fanout(SessionPlan& plan, const StreamFanoutPlan& fanout) {
    for (const auto& decision : fanout.branches) {
        for (auto& client : plan.clients) {
            if (client.client_id != decision.client_id ||
                client.connection_state != SessionConnectionState::Connected) {
                continue;
            }
            sync_applied_from_branch_decision(client, decision, plan);
            break;
        }
    }
}

void log_stream_fanout_plan(const StreamFanoutPlan& fanout, std::string_view reason) {
    std::cerr
        << "Stream fanout plan [" << stream_pipeline_action_name(fanout.trunk_action) << "]"
        << " trunk_changes=" << (fanout.trunk_changes ? "yes" : "no")
        << " streams=" << fanout.streams.size()
        << " branches=" << fanout.branches.size()
        << " reason=" << reason << '\n';
    for (const auto& stream : fanout.streams) {
        std::cerr
            << "  stream " << (stream.is_trunk ? "trunk" : "sample")
            << " owner=" << static_cast<int>(stream.owner_client_id)
            << " members=" << stream.member_client_ids.size()
            << " @" << stream.settings.bitrate_kbps << "kbps/"
            << static_cast<int>(stream.settings.framerate) << "fps";
        if (stream.settings.width > 0 && stream.settings.height > 0) {
            std::cerr << "/" << stream.settings.width << "x" << stream.settings.height;
        }
        std::cerr << '\n';
        for (const auto& step : migrations_for_stream(stream)) {
            std::cerr
                << "    client " << static_cast<int>(step.client_id)
                << " -> " << stream_branch_action_name(step.action);
            if (step.action == StreamBranchAction::ReuseClient) {
                std::cerr << " (owner " << static_cast<int>(step.stream_owner_client_id) << ")";
            }
            std::cerr << '\n';
        }
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
    MediaStreamFps fps = MediaStreamFps::Fps30;
};

PlayerEncodeContribution player_encode_contribution(
    const SessionClientConnection& client,
    std::uint16_t capture_width,
    std::uint16_t capture_height,
    MediaStreamSize size_override,
    MediaQualityTier tier_override,
    MediaStreamFeel feel_override,
    MediaStreamBitrate bitrate_override,
    MediaStreamFps fps_override,
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
    const MediaStreamFps fps = use_override ? fps_override : effective_fps_cap_for(client);
    return PlayerEncodeContribution{
        video_encode_settings_for_client(
            client,
            size,
            tier,
            capture_width,
            capture_height,
            feel,
            bitrate,
            fps),
        size,
        tier,
        feel,
        bitrate,
        fps,
    };
}

struct SessionVideoCeiling {
    VideoEncodeSettings settings{};
    MediaStreamSize size = MediaStreamSize::P720;
    MediaQualityTier tier = MediaQualityTier::Medium;
    MediaStreamFeel feel = MediaStreamFeel::LowLatency;
    MediaStreamBitrate bitrate = MediaStreamBitrate::Auto;
    MediaStreamFps fps = MediaStreamFps::Fps30;
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
    MediaStreamFps fps_override,
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
            fps_override,
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
            ceiling.fps = contrib.fps;
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
        if (stream_fps_rank(contrib.fps) > stream_fps_rank(ceiling.fps)) {
            ceiling.fps = contrib.fps;
        }
    }
    if (!ceiling.any_player) {
        if (plan.stream.video_configured) {
            ceiling.settings = plan.stream.video_settings;
            ceiling.size = plan.stream.video_size;
            ceiling.tier = plan.stream.video_tier;
            ceiling.feel = plan.stream.video_feel;
            ceiling.bitrate = plan.stream.video_bitrate;
            ceiling.fps = plan.stream.video_fps;
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
            ceiling.fps = MediaStreamFps::Fps30;
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
    configure_initial_session_video(plan_, capture_width_, capture_height_);
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
        if (plan_.game.session_mode == GameSessionMode::SinglePlayer) {
            return client_label(client) + " kicked; ending singleplayer session";
        }
        ++i;
    }

    if (plan_.control.soft_keyboard) {
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
            request = plan_.control.soft_keyboard->take_unsent_request();
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
                if (plan_.game.session_mode == GameSessionMode::SinglePlayer) {
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
                if (plan_.control.soft_keyboard) {
                    plan_.control.soft_keyboard->submit_response(*soft_keyboard);
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
                    if (emu_control->pause == EmulatorControlState::On) {
                        emulator_pause_requested_ = true;
                        for (auto& session_client : plan_.clients) {
                            session_client.video_zero_frame_streak = 0;
                        }
                    } else if (emu_control->pause == EmulatorControlState::Off) {
                        emulator_pause_requested_ = false;
                        for (auto& session_client : plan_.clients) {
                            session_client.video_zero_frame_streak = 0;
                        }
                    }
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
                        : plan_.game.save_username;
                    auto reply = handle_controls_db_packet(save_root_, claimed, payload);
                    if (!reply.empty()) {
                        client.stream.send_packet(reply);
                    }
                } catch (const std::exception&) {
                }
            } else if (is_pair_form_relay_packet(payload)) {
                try {
                    auto reply = handle_pair_form_relay_packet(payload);
                    if (!reply.empty()) {
                        client.stream.send_packet(reply);
                    }
                } catch (const std::exception&) {
                }
            } else if (const auto* video_ready = std::get_if<MediaVideoReady>(&payload);
                       video_ready != nullptr) {
                if (client.pending_video_uri.has_value()) {
                    if (video_ready->video_uri.empty()) {
                        media_server_.abort_video_tier_cutover(client.client_id);
                        client.pending_video_uri.reset();
                        client.pending_tier.reset();
                        client.pending_size.reset();
                        client.pending_feel.reset();
                        client.pending_bitrate.reset();
                        client.pending_fps.reset();
                        client.video_cutover_started = {};
                        std::cerr << "Video staging NACK from " << client_label(client) << '\n';
                    } else if (video_ready->video_uri == *client.pending_video_uri) {
                        const auto pending_size =
                            client.pending_size.value_or(client.applied_size);
                        const auto pending_tier =
                            client.pending_tier.value_or(client.applied_tier);
                        const auto pending_feel =
                            client.pending_feel.value_or(client.applied_feel);
                        const auto pending_bitrate =
                            client.pending_bitrate.value_or(client.applied_bitrate);
                        const auto pending_fps =
                            client.pending_fps.value_or(client.applied_fps);
                        const auto ceiling = compute_session_video_ceiling(
                            plan_,
                            capture_width_,
                            capture_height_,
                            client.client_id,
                            pending_size,
                            pending_tier,
                            pending_feel,
                            pending_bitrate,
                            pending_fps,
                            true);

                        std::vector<StreamBranchCandidate> candidates;
                        candidates.reserve(plan_.clients.size());
                        for (auto& other : plan_.clients) {
                            if (other.connection_state != SessionConnectionState::Connected ||
                                !other.hello.wants_video) {
                                continue;
                            }
                            StreamBranchCandidate candidate{};
                            candidate.client_id = other.client_id;
                            candidate.connected = true;
                            candidate.wants_video = true;
                            if (!client_is_seated_player(other)) {
                                candidate.target_settings = ceiling.settings;
                            } else if (other.client_id == client.client_id) {
                                candidate.target_settings = player_encode_contribution(
                                    other,
                                    capture_width_,
                                    capture_height_,
                                    pending_size,
                                    pending_tier,
                                    pending_feel,
                                    pending_bitrate,
                                    pending_fps,
                                    true).settings;
                            } else {
                                const auto other_current = stream_request_from_applied(other);
                                StreamRequest other_requested{
                                    other.wanted_size == MediaStreamSize::Auto
                                        ? other.applied_size
                                        : other.wanted_size,
                                    other.wanted_tier == MediaQualityTier::Auto
                                        ? other.applied_tier
                                        : select_video_tier(
                                              other.wanted_tier,
                                              other.applied_tier,
                                              other.max_bitrate_kbps),
                                    other.wanted_feel,
                                    other.wanted_bitrate,
                                    effective_fps_cap_for(other),
                                };
                                const auto other_resolution = resolve_stream_request_for_client(
                                    other,
                                    other_current,
                                    other_requested,
                                    stream_health_metrics_from_client(other));
                                candidate.target_settings = video_encode_settings_for_client(
                                    other,
                                    other_resolution.request.size == MediaStreamSize::Auto
                                        ? other.applied_size
                                        : other_resolution.request.size,
                                    other_resolution.request.tier == MediaQualityTier::Auto
                                        ? other.applied_tier
                                        : other_resolution.request.tier,
                                    capture_width_,
                                    capture_height_,
                                    other_resolution.request.feel,
                                    other_resolution.request.bitrate,
                                    other_resolution.request.fps);
                            }
                            candidates.push_back(candidate);
                        }

                        const auto fanout = build_stream_fanout_plan(
                            client,
                            true,
                            true,
                            plan_.stream.video_settings,
                            ceiling.settings,
                            ceiling.size,
                            ceiling.tier,
                            ceiling.feel,
                            ceiling.bitrate,
                            ceiling.fps,
                            std::move(candidates),
                            false,
                            false);
                        log_stream_fanout_plan(fanout, "trunk replace commit");

                        const bool trunk_only_promote =
                            fanout.streams.size() == 1 &&
                            fanout.streams.front().is_trunk &&
                            fanout.branches.size() == 1 &&
                            fanout.branches.front().action == StreamBranchAction::TakeTrunk;

                        bool committed = false;
                        if (trunk_only_promote &&
                            media_server_.complete_video_tier_cutover(
                                client.client_id,
                                video_ready->video_uri)) {
                            // Promote the already-warm encode; client keeps that URI.
                            plan_.stream.video_settings = fanout.proposed_trunk;
                            plan_.stream.video_size = fanout.proposed_size;
                            plan_.stream.video_tier = fanout.proposed_tier;
                            plan_.stream.video_feel = fanout.proposed_feel;
                            plan_.stream.video_bitrate = fanout.proposed_bitrate;
                            plan_.stream.video_fps = fanout.proposed_fps;
                            plan_.stream.video_configured = true;
                            sync_all_applied_from_fanout(plan_, fanout);

                            auto endpoint = client.media_endpoint.value_or(MediaEndpoint{});
                            endpoint.video_uri = video_ready->video_uri;
                            client.media_endpoint = endpoint;
                            send_media_endpoint_to_client(plan_, client.client_id, endpoint);
                            committed = true;
                            std::cerr
                                << "Trunk replace promoted warm encode for "
                                << client_label(client)
                                << " -> " << video_ready->video_uri << '\n';
                        } else {
                            // Sample branches (or promote failure): fall back to shared restart.
                            media_server_.abort_video_tier_cutover(client.client_id);
                            reset_video_stall_tracking(plan_);
                            if (media_server_.apply_video_branch_layout(
                                    fanout.proposed_trunk,
                                    branch_layout_client_settings(fanout))) {
                                plan_.stream.video_settings = fanout.proposed_trunk;
                                plan_.stream.video_size = fanout.proposed_size;
                                plan_.stream.video_tier = fanout.proposed_tier;
                                plan_.stream.video_feel = fanout.proposed_feel;
                                plan_.stream.video_bitrate = fanout.proposed_bitrate;
                                plan_.stream.video_fps = fanout.proposed_fps;
                                plan_.stream.video_configured = true;
                                sync_all_applied_from_fanout(plan_, fanout);
                                if (client.media_endpoint.has_value()) {
                                    send_media_endpoint_to_client(
                                        plan_,
                                        client.client_id,
                                        *client.media_endpoint);
                                }
                                committed = true;
                                std::cerr
                                    << "Trunk replace committed via branch layout for "
                                    << client_label(client)
                                    << " streams=" << fanout.streams.size() << '\n';
                            } else {
                                std::cerr
                                    << "Trunk replace commit failed for "
                                    << client_label(client) << '\n';
                            }
                        }

                        if (committed) {
                            const auto completed_at = std::chrono::steady_clock::now();
                            for (auto& other : plan_.clients) {
                                if (other.connection_state ==
                                    SessionConnectionState::Connected) {
                                    other.last_video_reconfigure = completed_at;
                                    other.last_video_stall_restart = completed_at;
                                    other.decode_pressure_streak = 0;
                                    other.video_zero_frame_streak = 0;
                                }
                            }
                        }

                        client.pending_video_uri.reset();
                        client.pending_tier.reset();
                        client.pending_size.reset();
                        client.pending_feel.reset();
                        client.pending_bitrate.reset();
                        client.pending_fps.reset();
                        client.video_cutover_started = {};
                        client.video_cutover_failures = 0;
                    } else {
                        media_server_.abort_video_tier_cutover(client.client_id);
                        client.pending_video_uri.reset();
                        client.pending_tier.reset();
                        client.pending_size.reset();
                        client.pending_feel.reset();
                        client.pending_bitrate.reset();
                        client.pending_fps.reset();
                        client.video_cutover_started = {};
                        std::cerr
                            << "Video staging ACK rejected from " << client_label(client)
                            << " uri=\"" << video_ready->video_uri << "\"\n";
                    }
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
                    outbound = plan_.link.coordinator.handle(
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
                            const auto start = plan_.link.cable.begin(
                                plan_.game.system_key,
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
                                    plan_.link.pending_promotion = true;
                                    plan_.link.pending_host_client_id = start.logical_host_client_id;
                                    plan_.link.pending_client_client_id = start.logical_client_client_id;
                                    plan_.link.pending_host_username = start.logical_host_username;
                                    plan_.link.pending_client_username = start.logical_client_username;
                                }
#if defined(ARCHSTREAMER_DEBUG_GB_LINK)
                                send_retroarch_netcmd(
                                    std::string("SHOW_MSG ") + "Link cable: dual GB ready",
                                    plan_.control.retroarch_netcmd_port);
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
                << " (staging warm-up timed out)\n";
            if (client.video_cutover_failures < 255) {
                ++client.video_cutover_failures;
            }
            if (client.video_cutover_failures >= 3) {
                client.video_cutover_suppressed = true;
                std::cerr
                    << "Video staging suppressed for " << client_label(client)
                    << " after repeated warm-up failures\n";
            }
            client.pending_video_uri.reset();
            client.pending_tier.reset();
            client.pending_size.reset();
            client.pending_feel.reset();
            client.pending_bitrate.reset();
            client.pending_fps.reset();
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
    if (want_framecount != plan_.control.framecount_osd_enabled) {
        plan_.control.framecount_osd_enabled = want_framecount;
        std::cout
            << "RetroArch Frames OSD "
            << (want_framecount ? "enabled" : "disabled")
            << " (client request)\n";
        if (!want_framecount) {
            plan_.control.framecount_osd_tick = 0;
        }
    }
    if (plan_.control.framecount_osd_enabled &&
        (plan_.control.framecount_osd_last_sent.time_since_epoch().count() == 0 ||
         now - plan_.control.framecount_osd_last_sent >= kFramecountOsdInterval)) {
        // RetroArch has no netcmd for framecount_show; SHOW_MSG is the live toggle path.
        // Changing text each tick also forces GL/Xvfb presents on static menus.
        const auto message = "Frames: " + std::to_string(plan_.control.framecount_osd_tick++);
        if (send_retroarch_netcmd(
                std::string("SHOW_MSG ") + message,
                plan_.control.retroarch_netcmd_port)) {
            plan_.control.framecount_osd_last_sent = now;
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
    client.decode_queue_p95_ms = heartbeat.decode_queue_p95_ms;
    client.decode_queue_max_ms = heartbeat.decode_queue_max_ms;
    client.au_queue_p95_ms = heartbeat.au_queue_p95_ms;
    client.last_loss_permille = heartbeat.loss_permille;
    client.last_frames_decoded_delta = heartbeat.frames_decoded_delta;

    if (client_is_tv(client) &&
        client.decode_queue_p95_ms != ViewerHeartbeatLatencyUnknownMs &&
        client.decode_queue_p95_ms >= kTvDecodePressureP95Ms &&
        (client.last_decode_pressure_log.time_since_epoch().count() == 0 ||
         now - client.last_decode_pressure_log >= kDecodePressureLogInterval)) {
        client.last_decode_pressure_log = now;
        std::cerr
            << "TV decode pressure on " << client_label(client)
            << ": queue_p95=" << client.decode_queue_p95_ms
            << "ms queue_max=" << client.decode_queue_max_ms
            << "ms au_p95=" << client.au_queue_p95_ms << "ms\n";
    }

    if (heartbeat.display_layout != DisplayLayoutPreference::Auto &&
        heartbeat.display_layout != client.display_layout) {
        client.display_layout = heartbeat.display_layout;
        client.hello.display_layout = heartbeat.display_layout;
        if (plan_.game.system_key == "nds") {
            apply_nds_screen_layout(heartbeat.display_layout);
        }
    } else if (heartbeat.display_layout != DisplayLayoutPreference::Auto) {
        client.display_layout = heartbeat.display_layout;
        client.hello.display_layout = heartbeat.display_layout;
    }

    if (!client.hello.wants_video) {
        return;
    }

    if (heartbeat.frames_decoded_delta > 0) {
        if (client.positive_video_heartbeats < 255) {
            ++client.positive_video_heartbeats;
        }
        const auto ready_needed = client_is_tv(client)
            ? kTvInitialVideoReadyHeartbeats
            : static_cast<std::uint8_t>(1);
        if (!client.initial_video_settings_ready &&
            client.positive_video_heartbeats >= ready_needed) {
            client.initial_video_settings_ready = true;
            if (client.initial_video_settings_defer_logged) {
                std::cerr
                    << "Initial video ready for " << client_label(client)
                    << " (" << static_cast<int>(client.positive_video_heartbeats)
                    << " positive frame heartbeat(s)); stream settings may apply\n";
            }
        }
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
    MediaStreamSize resolved_size =
        heartbeat.wanted_size == MediaStreamSize::Auto
            ? client.applied_size
            : heartbeat.wanted_size;
    MediaStreamFeel resolved_feel = heartbeat.wanted_feel;
    MediaStreamBitrate resolved_bitrate = heartbeat.wanted_bitrate;
    const bool bitrate_fixed = resolved_bitrate != MediaStreamBitrate::Auto;
    const bool hard_loss = heartbeat.loss_permille >= kHighLossPermille;

    auto resolved_requested_tier = heartbeat.wanted_tier != MediaQualityTier::Auto
        ? select_video_tier(heartbeat.wanted_tier, client.applied_tier, client.max_bitrate_kbps)
        : client.applied_tier;
    MediaStreamFps resolved_fps = effective_fps_cap_for(client);
    const auto current_request = stream_request_from_applied(client);
    const auto requested = StreamRequest{
        resolved_size,
        resolved_requested_tier,
        resolved_feel,
        resolved_bitrate,
        resolved_fps,
    };
    auto health = stream_health_metrics_from_heartbeat(heartbeat);
    // Handover / post-promote blanks and stale queue samples are not encode overload.
    const bool within_cutover_grace =
        client.pending_video_uri.has_value() ||
        media_server_.video_cutover_in_flight(client.client_id) ||
        (client.last_video_reconfigure.time_since_epoch().count() != 0 &&
         now - client.last_video_reconfigure < kPostReconfigureGrace);
    if (within_cutover_grace) {
        health.decode_queue_p95_ms = ViewerHeartbeatLatencyUnknownMs;
        health.decode_queue_max_ms = ViewerHeartbeatLatencyUnknownMs;
        health.au_queue_p95_ms = ViewerHeartbeatLatencyUnknownMs;
        client.decode_pressure_streak = 0;
    }
    const auto resolution = resolve_stream_request_for_client(
        client,
        current_request,
        requested,
        health);
    // "hold current" can fire every heartbeat while mildly pressurized — only
    // log actual step-downs / caps so the host log stays readable.
    log_stream_request_resolution(client, client_label(client), requested, resolution);
    resolved_size = resolution.request.size;
    resolved_requested_tier = resolution.request.tier;
    resolved_feel = resolution.request.feel;
    resolved_bitrate = resolution.request.bitrate;
    resolved_fps = resolution.request.fps;
    const bool requested_stream_change =
        resolved_requested_tier != client.applied_tier ||
        resolved_size != client.applied_size ||
        resolved_feel != client.applied_feel ||
        resolved_bitrate != client.applied_bitrate ||
        resolved_fps != client.applied_fps;
    if (recover_stalled_video_if_needed(client, heartbeat)) {
        return;
    }

    auto restage_reason = [&](
        MediaQualityTier tier,
        MediaStreamSize size,
        MediaStreamFeel feel,
        MediaStreamBitrate bitrate,
        MediaStreamFps fps) -> const char* {
        if (resolution.limited_by_metrics && !resolution.reason.empty()) {
            return resolution.reason.c_str();
        }
        const bool tier_changed = tier != client.applied_tier;
        const bool size_changed = size != client.applied_size;
        const bool feel_changed = feel != client.applied_feel;
        const bool bitrate_changed = bitrate != client.applied_bitrate;
        const bool fps_changed = fps != client.applied_fps;
        if (fps_changed && !tier_changed && !size_changed && !feel_changed && !bitrate_changed) {
            return "host TV FPS cap";
        }
        if (feel_changed && !tier_changed && !size_changed && !bitrate_changed && !fps_changed) {
            return "client requested stream feel";
        }
        if (bitrate_changed && !tier_changed && !size_changed && !feel_changed && !fps_changed) {
            return "client requested bitrate";
        }
        if (size_changed && !tier_changed && !feel_changed && !bitrate_changed && !fps_changed) {
            return "client requested size";
        }
        if (tier_changed && !size_changed && !feel_changed && !bitrate_changed && !fps_changed) {
            return "client requested frame rate";
        }
        return "client requested size/quality";
    };

    if (heartbeat.wanted_tier != MediaQualityTier::Auto) {
        const auto resolved = resolved_requested_tier;
        if (resolved != client.applied_tier ||
            resolved_size != client.applied_size ||
            resolved_feel != client.applied_feel ||
            resolved_bitrate != client.applied_bitrate ||
            resolved_fps != client.applied_fps) {
            apply_video_encode(
                client,
                resolved_size,
                resolved,
                resolved_feel,
                resolved_bitrate,
                resolved_fps,
                restage_reason(resolved, resolved_size, resolved_feel, resolved_bitrate, resolved_fps));
        }
        if (!resolution.limited_by_metrics) {
            client.bad_health_streak = 0;
            client.good_health_streak = 0;
        }
        return;
    }

    // Size / feel / bitrate / fps can still change under Auto frame rate.
    if (resolved_size != client.applied_size ||
        resolved_feel != client.applied_feel ||
        resolved_bitrate != client.applied_bitrate ||
        resolved_fps != client.applied_fps) {
        apply_video_encode(
            client,
            resolved_size,
            client.applied_tier,
            resolved_feel,
            resolved_bitrate,
            resolved_fps,
            restage_reason(
                client.applied_tier, resolved_size, resolved_feel, resolved_bitrate, resolved_fps));
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
            client.applied_fps,
            bitrate_fixed ? "auto ceiling (frame rate)" : "auto ceiling (cap High/Very-High)");
        return;
    }

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
                    client.applied_fps,
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
            client.applied_fps,
            "auto step-up (healthy)");
        client.good_health_streak = 0;
    }
}

bool SessionControlMonitor::recover_stalled_video_if_needed(
    SessionClientConnection& client,
    const ViewerHeartbeat& heartbeat) {
    if (heartbeat.frames_decoded_delta > 0) {
        client.video_zero_frame_streak = 0;
        return false;
    }
    // Warm cutover / pending URI: zero frames on the old or new path are normal.
    if (!client_is_seated_player(client) ||
        !plan_.stream.video_configured ||
        emulator_pause_requested_ ||
        client.pending_video_uri.has_value() ||
        media_server_.video_cutover_in_flight(client.client_id)) {
        client.video_zero_frame_streak = 0;
        return false;
    }

    const auto now = std::chrono::steady_clock::now();
    // Right after promote/reconfigure the client may still report blanks while
    // the new encode starts (or while UI rebinds). Do not accumulate those.
    if (client.last_video_reconfigure.time_since_epoch().count() != 0 &&
        now - client.last_video_reconfigure < kPostReconfigureStallGrace) {
        client.video_zero_frame_streak = 0;
        return false;
    }

    if (client.video_zero_frame_streak < 255) {
        ++client.video_zero_frame_streak;
    }

    if (client.last_video_reconfigure.time_since_epoch().count() == 0) {
        return false;
    }
    const auto since_reconfigure = now - client.last_video_reconfigure;
    const bool in_reconfigure_recovery_window =
        since_reconfigure >= kPostReconfigureStallGrace &&
        since_reconfigure <= kPostReconfigureStallWindow;
    if (!in_reconfigure_recovery_window) {
        return false;
    }

    // Low loss + zero frames ≈ local decoder blank (packets still arriving).
    // High loss ≈ dead/broken pipe — recover sooner.
    const bool pipe_looks_dead = heartbeat.loss_permille >= kHighLossPermille;
    const auto restart_threshold = pipe_looks_dead
        ? kDeadPipeHighLossStallThreshold
        : kDeadPipeLowLossStallThreshold;
    if (client.video_zero_frame_streak < restart_threshold) {
        return false;
    }

    if (client.last_video_stall_restart.time_since_epoch().count() != 0 &&
        now - client.last_video_stall_restart < kVideoStallRestartInterval) {
        return false;
    }

    std::cerr
        << "Session video post-reconfigure stall on " << client_label(client)
        << " (" << static_cast<int>(client.video_zero_frame_streak)
        << " zero-frame heartbeats, loss=" << heartbeat.loss_permille
        << "‰); restarting shared media fanout\n";
    if (!media_server_.reconfigure_shared_video(plan_.stream.video_settings)) {
        return false;
    }
    if (media_server_.restart_shared_audio()) {
        std::cerr << "Session audio fanout restarted after video stall\n";
    }

    // Promote leaves clients on dedicated/staging ports; reconfigure_shared moves
    // them back to base. Resend MediaEndpoint or they keep listening on a dead port.
    for (auto& other : plan_.clients) {
        if (other.connection_state != SessionConnectionState::Connected ||
            !other.hello.wants_video) {
            continue;
        }
        const auto video_uri = media_server_.current_video_uri(other.client_id);
        if (!video_uri.has_value()) {
            continue;
        }
        auto endpoint = other.media_endpoint.value_or(MediaEndpoint{});
        if (endpoint.video_uri == *video_uri) {
            continue;
        }
        endpoint.video_uri = *video_uri;
        other.media_endpoint = endpoint;
        send_media_endpoint_to_client(plan_, other.client_id, endpoint);
        std::cerr
            << "Stall recovery retargeted video for " << client_label(other)
            << " -> " << *video_uri << '\n';
    }

    const auto completed_at = std::chrono::steady_clock::now();

    reset_video_stall_tracking(plan_);
    for (auto& other : plan_.clients) {
        if (other.connection_state == SessionConnectionState::Connected) {
            other.last_video_reconfigure = completed_at;
            other.last_video_stall_restart = completed_at;
            other.decode_pressure_streak = 0;
        }
    }
    return true;
}

void SessionControlMonitor::apply_video_encode(
    SessionClientConnection& client,
    MediaStreamSize size,
    MediaQualityTier tier,
    MediaStreamFeel feel,
    MediaStreamBitrate bitrate,
    MediaStreamFps fps,
    std::string_view reason) {
    const auto now = std::chrono::steady_clock::now();
    const bool cutover_in_flight =
        client.pending_video_uri.has_value() ||
        media_server_.video_cutover_in_flight(client.client_id);
    const bool within_reconfigure_cooldown =
        client.last_video_reconfigure.time_since_epoch().count() != 0 &&
        now - client.last_video_reconfigure < kMinReconfigureInterval;

    if (size == MediaStreamSize::Auto) {
        size = client.applied_size;
    }
    auto resolved = select_video_tier(tier, client.applied_tier, client.max_bitrate_kbps);
    const auto current_request = stream_request_from_applied(client);
    const auto requested = StreamRequest{size, resolved, feel, bitrate, fps};
    const auto resolution = resolve_stream_request_for_client(
        client,
        current_request,
        requested,
        stream_health_metrics_from_client(client));
    log_stream_request_resolution(client, client_label(client), requested, resolution);
    size = resolution.request.size;
    resolved = resolution.request.tier;
    feel = resolution.request.feel;
    bitrate = resolution.request.bitrate;
    fps = resolution.request.fps;
    const auto ceiling = compute_session_video_ceiling(
        plan_,
        capture_width_,
        capture_height_,
        client.client_id,
        size,
        resolved,
        feel,
        bitrate,
        fps,
        true);

    // Branch targets are resolved against the proposed trunk (destination state).
    std::vector<StreamBranchCandidate> candidates;
    candidates.reserve(plan_.clients.size());
    for (auto& other : plan_.clients) {
        if (other.connection_state != SessionConnectionState::Connected ||
            !other.hello.wants_video) {
            continue;
        }
        StreamBranchCandidate candidate{};
        candidate.client_id = other.client_id;
        candidate.connected = true;
        candidate.wants_video = true;
        if (!client_is_seated_player(other)) {
            // Viewers never raise the trunk; they ride it (or a later sample of it).
            candidate.target_settings = ceiling.settings;
        } else if (other.client_id == client.client_id) {
            candidate.target_settings = player_encode_contribution(
                other,
                capture_width_,
                capture_height_,
                size,
                resolved,
                feel,
                bitrate,
                fps,
                true).settings;
        } else {
            const auto other_current = stream_request_from_applied(other);
            StreamRequest other_requested{
                other.wanted_size == MediaStreamSize::Auto ? other.applied_size : other.wanted_size,
                other.wanted_tier == MediaQualityTier::Auto
                    ? other.applied_tier
                    : select_video_tier(other.wanted_tier, other.applied_tier, other.max_bitrate_kbps),
                other.wanted_feel,
                other.wanted_bitrate,
                effective_fps_cap_for(other),
            };
            const auto other_resolution = resolve_stream_request_for_client(
                other,
                other_current,
                other_requested,
                stream_health_metrics_from_client(other));
            candidate.target_settings = video_encode_settings_for_client(
                other,
                other_resolution.request.size == MediaStreamSize::Auto
                    ? other.applied_size
                    : other_resolution.request.size,
                other_resolution.request.tier == MediaQualityTier::Auto
                    ? other.applied_tier
                    : other_resolution.request.tier,
                capture_width_,
                capture_height_,
                other_resolution.request.feel,
                other_resolution.request.bitrate,
                other_resolution.request.fps);
        }
        candidates.push_back(candidate);
    }

    const auto fanout = build_stream_fanout_plan(
        client,
        client_is_seated_player(client),
        plan_.stream.video_configured,
        plan_.stream.video_settings,
        ceiling.settings,
        ceiling.size,
        ceiling.tier,
        ceiling.feel,
        ceiling.bitrate,
        ceiling.fps,
        std::move(candidates),
        cutover_in_flight,
        within_reconfigure_cooldown);
    if (fanout.trunk_action == StreamPipelineAction::None) {
        return;
    }

    // Wait for real decoded frames before raising/replacing/reshaping media.
    // Manual mid-session changes still apply once initial_video_settings_ready.
    if (!client.initial_video_settings_ready &&
        (fanout.trunk_action == StreamPipelineAction::ReplaceTrunk ||
         fanout.trunk_action == StreamPipelineAction::HardRestartTrunk ||
         fanout.trunk_action == StreamPipelineAction::SampleFromTrunk)) {
        if (!client.initial_video_settings_defer_logged) {
            client.initial_video_settings_defer_logged = true;
            std::cerr
                << "Deferring stream change until video is stable for "
                << client_label(client)
                << " [" << stream_pipeline_action_name(fanout.trunk_action) << "]\n";
        }
        return;
    }

    client.bad_health_streak = 0;
    client.good_health_streak = 0;
    log_stream_fanout_plan(fanout, reason);

    auto commit_session_trunk = [&]() {
        plan_.stream.video_settings = fanout.proposed_trunk;
        plan_.stream.video_size = fanout.proposed_size;
        plan_.stream.video_tier = fanout.proposed_tier;
        plan_.stream.video_feel = fanout.proposed_feel;
        plan_.stream.video_bitrate = fanout.proposed_bitrate;
        plan_.stream.video_fps = fanout.proposed_fps;
        plan_.stream.video_configured = true;
    };

    auto apply_layout = [&]() -> bool {
        reset_video_stall_tracking(plan_);
        if (!media_server_.apply_video_branch_layout(
                fanout.proposed_trunk,
                branch_layout_client_settings(fanout))) {
            return false;
        }
        commit_session_trunk();
        sync_all_applied_from_fanout(plan_, fanout);
        const auto completed_at = std::chrono::steady_clock::now();
        for (auto& other : plan_.clients) {
            if (other.connection_state == SessionConnectionState::Connected) {
                other.last_video_reconfigure = completed_at;
                other.last_video_stall_restart = completed_at;
                other.decode_pressure_streak = 0;
                other.video_zero_frame_streak = 0;
            }
        }
        return true;
    };

    auto begin_trunk_replace = [&]() {
        if (const auto staging_uri = media_server_.begin_video_tier_cutover(
                client.client_id,
                fanout.proposed_trunk);
            staging_uri.has_value()) {
            client.pending_video_uri = *staging_uri;
            client.pending_tier = fanout.proposed_tier;
            client.pending_size = fanout.proposed_size;
            client.pending_feel = fanout.proposed_feel;
            client.pending_bitrate = fanout.proposed_bitrate;
            client.pending_fps = fanout.proposed_fps;
            client.video_cutover_started = now;
            try {
                client.stream.send_packet(serialize_packet(MediaVideoPending{*staging_uri}));
                std::cerr
                    << "Trunk replace warm-up for " << client_label(client)
                    << " uri=" << *staging_uri
                    << " streams=" << fanout.streams.size()
                    << " [" << stream_pipeline_action_name(StreamPipelineAction::ReplaceTrunk)
                    << "]\n";
                return true;
            } catch (const std::exception& error) {
                media_server_.abort_video_tier_cutover(client.client_id);
                client.pending_video_uri.reset();
                client.pending_tier.reset();
                client.pending_size.reset();
                client.pending_feel.reset();
                client.pending_bitrate.reset();
                client.pending_fps.reset();
                client.video_cutover_started = {};
                std::cerr
                    << "Trunk replace notify failed for " << client_label(client)
                    << ": " << error.what() << "; falling back to hard restart\n";
            }
        }
        return false;
    };

    switch (fanout.trunk_action) {
    case StreamPipelineAction::None:
        return;
    case StreamPipelineAction::SyncToTrunk:
        // Trunk unchanged and everyone rides it — no media restart.
        sync_applied_to_session(client, plan_);
        return;
    case StreamPipelineAction::SampleFromTrunk:
        // Trunk unchanged; reshape sample/reuse branches only.
        if (!apply_layout()) {
            return;
        }
        std::cerr
            << "Branch layout refreshed without trunk replace from "
            << client_label(client) << ": " << reason << '\n';
        return;
    case StreamPipelineAction::ReplaceTrunk:
        if (begin_trunk_replace()) {
            return;
        }
        [[fallthrough]];
    case StreamPipelineAction::HardRestartTrunk:
        if (!apply_layout()) {
            return;
        }
        std::cerr
            << "Session trunk committed -> "
            << media_stream_size_name(fanout.proposed_size) << "/"
            << media_quality_tier_name(fanout.proposed_tier) << "/"
            << media_stream_bitrate_name(fanout.proposed_bitrate) << "/"
            << media_stream_feel_name(fanout.proposed_feel) << "/"
            << media_stream_fps_name(fanout.proposed_fps)
            << " from " << client_label(client)
            << ": " << reason
            << " [" << stream_pipeline_action_name(fanout.trunk_action) << "]\n";
        return;
    }
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
    plan_.link.coordinator.clear_client(plan_.clients[index].client_id);
    if (host_hub_ != nullptr) {
        host_hub_->clear_link_client(plan_.clients[index].client_id);
    }
    if (plan_.clients[index].client_id == plan_.link.cable.client_a() ||
        plan_.clients[index].client_id == plan_.link.cable.client_b()) {
        plan_.link.cable.clear();
    }
    media_server_.remove_client(plan_.clients[index].client_id);
    clear_connected_client(save_root_, plan_.clients[index].client_id, slot_index_);
    plan_.clients.erase(plan_.clients.begin() + static_cast<std::ptrdiff_t>(index));
    record_client_left(
        slot_index_,
        username,
        plan_.game.selected_game_id,
        std::string(reason),
        session_id_);
    return true;
}

void SessionControlMonitor::mark_player_disconnected(SessionClientConnection& client, std::string_view reason) {
    plan_.link.coordinator.clear_client(client.client_id);
    if (host_hub_ != nullptr) {
        host_hub_->clear_link_client(client.client_id);
    }
    if (client.client_id == plan_.link.cable.client_a() ||
        client.client_id == plan_.link.cable.client_b()) {
        plan_.link.cable.clear();
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
    client.pending_fps.reset();
    client.pending_video_uri.reset();
    client.video_cutover_started = {};
    input_router_.neutralize_client(client.client_id);

    // Drop this seat's contribution; remaining players own the ceiling.
    if (plan_.stream.video_configured && client.hello.wants_video) {
        const auto ceiling = compute_session_video_ceiling(
            plan_,
            capture_width_,
            capture_height_,
            client.client_id,
            MediaStreamSize::P720,
            MediaQualityTier::Medium,
            MediaStreamFeel::LowLatency,
            MediaStreamBitrate::Auto,
            MediaStreamFps::Fps30,
            false);
        if (ceiling.settings != plan_.stream.video_settings) {
            if (media_server_.reconfigure_shared_video(ceiling.settings)) {
                plan_.stream.video_settings = ceiling.settings;
                plan_.stream.video_size = ceiling.size;
                plan_.stream.video_tier = ceiling.tier;
                plan_.stream.video_feel = ceiling.feel;
                plan_.stream.video_bitrate = ceiling.bitrate;
                plan_.stream.video_fps = ceiling.fps;
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
        plan_.game.selected_game_id,
        std::string(reason),
        session_id_);
}

std::string SessionControlMonitor::client_label(const SessionClientConnection& client) {
    std::ostringstream out;
    out << "client " << static_cast<int>(client.client_id) << " (" << client.hello.username << ")";
    return out.str();
}

} // namespace archstreamer
