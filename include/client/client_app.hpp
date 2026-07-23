#pragma once

#include "client/controller_manager.hpp"
#include "client/game_filter.hpp"
#include "client/session_service.hpp"
#include "common/participant_role.hpp"
#include "common/protocol.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace archstreamer {

using ClientParticipantRole = ParticipantRole;

struct ClientAppConfig {
    std::string host = "127.0.0.1";
    std::uint16_t control_port = 45555;
    std::optional<std::uint16_t> input_port;
    std::string username;
    std::string display_name;
    ClientParticipantRole role = ClientParticipantRole::Player;
    GameFilter filter;
    GameSessionMode session_mode = GameSessionMode::SinglePlayer;
    std::optional<std::string> game_selector;
    std::vector<std::size_t> controller_indexes;
    bool wants_video = true;
    bool wants_audio = true;
    MediaQualityTier wanted_tier = MediaQualityTier::Auto;
    // 0 = use tier default bitrate cap on the host.
    std::uint16_t max_bitrate_kbps = 0;
};

struct ClientConnectionInfo {
    ClientId client_id = 0;
    std::string username;
    ClientParticipantRole role = ClientParticipantRole::Player;
    GameSessionMode session_mode = GameSessionMode::SinglePlayer;
    std::optional<GameId> selected_game_id;
};

struct ClientAppCallbacks {
    std::function<void(const GameList& full_catalog, const GameList& filtered_catalog)> on_catalog;
    std::function<void(const ClientConnectionInfo& connection)> on_connected;
    std::function<void(const SeatAssignment& seats)> on_seat_assignment;
    std::function<void(const SessionReady& ready)> on_session_ready;
    std::function<void(const MediaEndpoint& endpoint)> on_media_endpoint;
    std::function<void(const SessionStarting& starting)> on_session_starting;
    std::function<void(const std::string& reason)> on_session_ended;
    std::function<void()> on_host_disconnected;
    std::function<void(const std::string& host, std::uint16_t input_port)> on_input_streaming_started;
    std::function<void()> on_waiting_without_input;
    std::function<void(const std::string& message)> on_status;
};

struct ClientRunResult {
    std::optional<ClientId> client_id;
    GameList full_catalog;
    GameList filtered_catalog;
    std::optional<GameId> selected_game_id;
    SeatAssignment seats;
    SessionReady ready;
    SessionStarting starting;
    std::optional<MediaEndpoint> media_endpoint;
    bool host_disconnected = false;
    std::optional<std::string> ended_reason;
};

struct ClientSessionDraft {
    PendingSession pending_session;
    GameList filtered_catalog;
};

struct ClientCatalogView {
    GameList full_catalog;
    GameList filtered_catalog;
    std::filesystem::path art_cache_root;
};

class ClientApp {
public:
    std::vector<ControllerDevice> list_controllers() const;
    ActiveSessionInfo active_session_info(const std::string& host, std::uint16_t control_port) const;

    ClientCatalogView fetch_catalog(
        const ClientAppConfig& config,
        const ClientAppCallbacks& callbacks = {}) const;
    ClientSessionDraft begin_session(
        const ClientAppConfig& config,
        const ClientAppCallbacks& callbacks = {}) const;
    ClientRunResult join_session(
        ClientSessionDraft draft,
        const ClientAppConfig& config,
        const std::function<bool()>& should_stop,
        const ClientAppCallbacks& callbacks = {}) const;
    ClientRunResult run_session(
        const ClientAppConfig& config,
        const std::function<bool()>& should_stop,
        const ClientAppCallbacks& callbacks = {}) const;
};

std::optional<GameId> select_game_id(const GameList& list, const std::optional<std::string>& selector);
bool contains_game_id(const GameList& list, const GameId& game_id);

} // namespace archstreamer
