#pragma once

#include "common/serialization.hpp"
#include "common/platform/default_platform.hpp"
#include "host/linux_uinput_gamepad.hpp"
#include "host/seat_manager.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
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
    MediaQualityTier wanted_tier = MediaQualityTier::Auto;
    MediaQualityTier applied_tier = MediaQualityTier::Medium;
    std::uint16_t max_bitrate_kbps = 0;
    std::uint8_t bad_health_streak = 0;
    std::uint8_t good_health_streak = 0;
    std::chrono::steady_clock::time_point last_video_reconfigure = {};
};

struct SessionPlan {
    std::vector<SessionClientConnection> clients;
    std::optional<ClientHello> host_hello;
    SeatAssignment seats;
    GameId selected_game_id;
    GameSessionMode session_mode = GameSessionMode::SinglePlayer;
    std::string save_username;
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
void send_session_ended_to_clients(SessionPlan& plan, std::string_view reason);
const SessionClientConnection* session_client_for(const SessionPlan& plan, ClientId client_id);
std::optional<ControllerInfo> controller_for(const ClientHello& hello, LocalPlayerIndex local_player);
std::string sanitize_virtual_device_text(std::string_view value);
std::string controller_name_for(const ClientHello& hello, LocalPlayerIndex local_player);
std::vector<VirtualGamepadIdentity> virtual_identities_for_session(const SessionPlan& plan);

SessionPlan wait_for_session_clients(
    std::uint16_t control_port,
    std::uint8_t client_count,
    const GameList& game_list,
    std::chrono::seconds timeout,
    std::optional<ClientHello> host_hello = std::nullopt,
    std::function<bool()> should_stop = {},
    std::filesystem::path art_root = {});

} // namespace archstreamer
