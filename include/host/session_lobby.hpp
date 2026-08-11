#pragma once

#include "common/serialization.hpp"
#include "common/platform/default_platform.hpp"
#include "host/link_cable_backend.hpp"
#include "host/link_coordinator.hpp"
#include "host/soft_keyboard_host.hpp"
#include "host/virtual_gamepad.hpp"
#include "host/retroarch_netcmd.hpp"
#include "host/seat_manager.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace archstreamer {

enum class SessionConnectionState {
    Connected,
    Disconnected,
};

struct SessionClientConnection {
    ClientId client_id = 0;
    ClientHello hello;
    TcpStream stream;
    SessionConnectionState connection_state = SessionConnectionState::Connected;
    std::chrono::steady_clock::time_point last_seen = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point disconnected_at = {};
    // "left" (ClientSessionLeave) vs "disconnected" (TCP close) vs "heartbeat timed out".
    std::string disconnect_reason;
    MediaQualityTier wanted_tier = MediaQualityTier::Auto;
    MediaQualityTier applied_tier = MediaQualityTier::Medium;
    MediaStreamSize wanted_size = MediaStreamSize::Auto;
    MediaStreamSize applied_size = MediaStreamSize::P720;
    MediaStreamFeel wanted_feel = MediaStreamFeel::LowLatency;
    MediaStreamFeel applied_feel = MediaStreamFeel::LowLatency;
    MediaStreamBitrate wanted_bitrate = MediaStreamBitrate::Auto;
    MediaStreamBitrate applied_bitrate = MediaStreamBitrate::Auto;
    MediaStreamFps adaptive_fps_cap = MediaStreamFps::Auto;
    MediaStreamFps applied_fps = MediaStreamFps::Fps30;
    DisplayLayoutPreference display_layout = DisplayLayoutPreference::Auto;
    /** Tier/size/feel/bitrate being warmed on a staging RTP path (cutover in flight). */
    std::optional<MediaQualityTier> pending_tier;
    std::optional<MediaStreamSize> pending_size;
    std::optional<MediaStreamFeel> pending_feel;
    std::optional<MediaStreamBitrate> pending_bitrate;
    std::optional<MediaStreamFps> pending_fps;
    std::optional<std::string> pending_video_uri;
    std::chrono::steady_clock::time_point video_cutover_started = {};
    std::uint16_t max_bitrate_kbps = 0;
    bool show_framecount = false;
    std::uint8_t bad_health_streak = 0;
    std::uint8_t good_health_streak = 0;
    // After Auto steps down from High due to loss/no frames, hold off retrying High.
    std::chrono::steady_clock::time_point high_tier_cooldown_until = {};
    std::chrono::steady_clock::time_point last_video_reconfigure = {};
    std::uint8_t video_zero_frame_streak = 0;
    std::chrono::steady_clock::time_point last_video_stall_restart = {};
    std::chrono::steady_clock::time_point last_decode_pressure_log = {};
    std::uint16_t decode_queue_p95_ms = ViewerHeartbeatLatencyUnknownMs;
    std::uint16_t decode_queue_max_ms = ViewerHeartbeatLatencyUnknownMs;
    std::uint16_t au_queue_p95_ms = ViewerHeartbeatLatencyUnknownMs;
    /** Last heartbeat loss / decode deltas — used when resolving stream requests. */
    std::uint16_t last_loss_permille = 0;
    std::uint16_t last_frames_decoded_delta = 0;
    std::uint8_t decode_pressure_streak = 0;
    std::uint8_t positive_video_heartbeats = 0;
    bool initial_video_settings_ready = false;
    bool initial_video_settings_defer_logged = false;
    // Consecutive MediaVideoPending timeouts without MediaVideoReady (e.g. older
    // clients). After a few, stop staging size changes until reconnect.
    std::uint8_t video_cutover_failures = 0;
    bool video_cutover_suppressed = false;
    // Last advertised RTP endpoints (resent after video ladder restart so the
    // client can one-shot resync A/V without the host bouncing shared audio).
    std::optional<MediaEndpoint> media_endpoint;
};

struct SessionLinkState {
    // Mid-session mutual link matchmaking (backends wired later).
    LinkCoordinator coordinator;
    LinkCableBackend cable;
    /**
     * Set by SessionControlMonitor when a match needs SessionRuntime promotion.
     * Consumed by host_app (fields mirror LinkPromotionRequest).
     */
    bool pending_promotion = false;
    ClientId pending_host_client_id = 0;
    ClientId pending_client_client_id = 0;
    std::string pending_host_username;
    std::string pending_client_username;
};

struct SessionStreamState {
    /**
     * Single shared video encode for this session slot.
     * Ceiling is owned by seated players only; viewers/Watch receive this bitstream.
     */
    VideoEncodeSettings video_settings = video_encode_settings(MediaStreamSize::P720, MediaQualityTier::Medium);
    MediaStreamSize video_size = MediaStreamSize::P720;
    MediaQualityTier video_tier = MediaQualityTier::Medium;
    MediaStreamFeel video_feel = MediaStreamFeel::LowLatency;
    MediaStreamBitrate video_bitrate = MediaStreamBitrate::Auto;
    MediaStreamFps video_fps = MediaStreamFps::Fps30;
    bool video_configured = false;
};

struct SessionPlan {
    std::vector<SessionClientConnection> clients;
    std::optional<ClientHello> host_hello;
    SeatAssignment seats;
    GameId selected_game_id;
    GameSessionMode session_mode = GameSessionMode::SinglePlayer;
    std::string save_username;
    std::string system_key;
    // Multi-disc playlist state (from launched .m3u); empty when not applicable.
    std::vector<std::string> playlist_discs;
    std::uint8_t current_disc_index = 0;
    std::uint16_t retroarch_netcmd_port = DefaultRetroArchNetcmdPort;
    // Client-requested RetroArch Frames OSD (OR of heartbeats); driven via SHOW_MSG.
    bool framecount_osd_enabled = false;
    std::uint32_t framecount_osd_tick = 0;
    std::chrono::steady_clock::time_point framecount_osd_last_sent = {};
    SessionLinkState link;
    /** Pad OSK for Ryujinx Software Keyboard (optional; set for Switch sessions). */
    std::shared_ptr<SoftKeyboardHostBridge> soft_keyboard;
    SessionStreamState stream;
};

const char* session_mode_name(GameSessionMode mode);
PacketPayload receive_control_payload(TcpStream& stream);
std::optional<GameInfo> game_info_for(const GameList& list, const GameId& game_id);
GameList catalog_delta_for_request(const GameList& full_list, const GameListRequest& request);
RetroArchPort assigned_player_count(const SeatAssignment& seats);
std::uint8_t requested_player_count(const SessionPlan& plan);
ActiveSessionInfo active_session_info_for(
    const SessionPlan& plan,
    bool video_enabled,
    bool audio_enabled);
std::uint8_t required_player_count(GameSessionMode mode, const GameInfo& game);
bool launch_requirements_satisfied(const SessionPlan& plan, const GameInfo& game);
void send_error_to_session_clients(SessionPlan& plan, std::string_view message);
void send_session_ready_to_clients(SessionPlan& plan);
void send_session_starting_to_clients(SessionPlan& plan);
void send_media_endpoint_to_client(SessionPlan& plan, ClientId client_id, const MediaEndpoint& endpoint);
void configure_initial_session_video(
    SessionPlan& plan,
    std::uint16_t capture_width,
    std::uint16_t capture_height);
void send_session_ended_to_clients(SessionPlan& plan, std::string_view reason);
DiscControlResponse apply_disc_control(SessionPlan& plan, const DiscControlRequest& request);
const SessionClientConnection* session_client_for(const SessionPlan& plan, ClientId client_id);
std::optional<ControllerInfo> controller_for(const ClientHello& hello, LocalPlayerIndex local_player);
std::string sanitize_virtual_device_text(std::string_view value);
std::string controller_name_for(const ClientHello& hello, LocalPlayerIndex local_player);
std::vector<VirtualGamepadIdentity> virtual_identities_for_session(const SessionPlan& plan);
void log_client_hello(ClientId client_id, const ClientHello& hello);

SessionPlan gather_session_clients(
    TcpListener& listener,
    std::uint8_t client_count,
    const GameList& game_list,
    std::chrono::seconds timeout,
    std::optional<ClientHello> host_hello = std::nullopt,
    std::function<bool()> should_stop = {},
    std::filesystem::path art_root = {},
    std::optional<SessionClientConnection> first_client = std::nullopt,
    std::filesystem::path save_root = {},
    bool allow_new_users = false);

/** Assign seats, send HostWelcome/SeatAssignment/SessionReady, set save_username. */
void finalize_session_plan_ready(SessionPlan& plan);

/** Build a ready Singleplayer plan from one seated player client. */
SessionPlan make_singleplayer_session_plan(
    ClientId client_id,
    ClientHello hello,
    TcpStream stream,
    const GameList& game_list,
    const std::filesystem::path& save_root = {});

} // namespace archstreamer
