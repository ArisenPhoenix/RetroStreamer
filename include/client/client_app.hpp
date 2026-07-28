#pragma once

#include "client/controller_manager.hpp"
#include "client/game_filter.hpp"
#include "client/session_service.hpp"
#include "common/participant_role.hpp"
#include "common/protocol.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace archstreamer {

using ClientParticipantRole = ParticipantRole;

// Shared between the GUI thread and the client session loop so Settings can change
// heartbeat fields (quality / debug overlay) while a stream is already running.
struct ClientHeartbeatPrefs {
    std::mutex mutex;
    MediaQualityTier wanted_tier = MediaQualityTier::Auto;
    std::uint16_t max_bitrate_kbps = 0;
    bool show_framecount = false;

    void set_wanted_tier(MediaQualityTier tier) {
        std::lock_guard lock(mutex);
        wanted_tier = tier;
    }

    void set_show_framecount(bool enabled) {
        std::lock_guard lock(mutex);
        show_framecount = enabled;
    }

    void snapshot(MediaQualityTier& tier, std::uint16_t& max_bitrate, bool& framecount) {
        std::lock_guard lock(mutex);
        tier = wanted_tier;
        max_bitrate = max_bitrate_kbps;
        framecount = show_framecount;
    }
};

// Shared between the GUI thread and the client session loop for mid-session disc swaps.
struct DiscControlBridge {
    std::mutex mutex;
    std::optional<DiscControlRequest> pending_request;
    std::optional<DiscControlResponse> last_response;
    std::vector<std::string> disc_labels;
    GameId active_game_id;
    bool session_active = false;

    void request_set_index(std::uint8_t index) {
        std::lock_guard lock(mutex);
        pending_request = DiscControlRequest{active_game_id, DiscControlAction::SetIndex, index};
    }

    void request_next() {
        std::lock_guard lock(mutex);
        pending_request = DiscControlRequest{active_game_id, DiscControlAction::Next, 0};
    }

    void request_prev() {
        std::lock_guard lock(mutex);
        pending_request = DiscControlRequest{active_game_id, DiscControlAction::Prev, 0};
    }

    std::optional<DiscControlRequest> take_pending() {
        std::lock_guard lock(mutex);
        auto request = pending_request;
        pending_request.reset();
        return request;
    }

    void set_response(DiscControlResponse response) {
        std::lock_guard lock(mutex);
        last_response = std::move(response);
    }

    std::optional<DiscControlResponse> take_response() {
        std::lock_guard lock(mutex);
        auto response = last_response;
        last_response.reset();
        return response;
    }
};

// Shared between the GUI thread and the client session loop for mutual link requests.
struct LinkControlBridge {
    std::mutex mutex;
    std::optional<LinkRequest> pending_request;
    std::optional<LinkResponse> last_response;
    GameId active_game_id;
    std::string system_key;
    bool session_active = false;
    bool link_capable = false;

    void request_link(std::string target_username) {
        std::lock_guard lock(mutex);
        pending_request = LinkRequest{
            active_game_id,
            std::move(target_username),
            LinkAction::Request,
        };
    }

    void cancel_link() {
        std::lock_guard lock(mutex);
        pending_request = LinkRequest{active_game_id, {}, LinkAction::Cancel};
    }

    std::optional<LinkRequest> take_pending() {
        std::lock_guard lock(mutex);
        auto request = pending_request;
        pending_request.reset();
        return request;
    }

    void set_response(LinkResponse response) {
        std::lock_guard lock(mutex);
        last_response = std::move(response);
    }

    std::optional<LinkResponse> take_response() {
        std::lock_guard lock(mutex);
        auto response = last_response;
        last_response.reset();
        return response;
    }
};

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
    // Remotes Space/arrows/Enter/Esc/Tab/Backspace/F1 to a host virtual keyboard.
    // Default on so kids get hold-to-fast-forward (Space) without extra setup.
    bool send_keyboard = true;
    // Experimental: one shared-clock GStreamer pipeline for A/V lip-sync.
    // Default false keeps the working dual gst-launch receivers.
    bool synced_av = false;
    MediaQualityTier wanted_tier = MediaQualityTier::Auto;
    // 0 = use tier default bitrate cap on the host.
    std::uint16_t max_bitrate_kbps = 0;
    // Request host RetroArch "Frames:" OSD (default off). Prefer heartbeat_prefs for live updates.
    bool show_framecount = false;
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
    std::shared_ptr<DiscControlBridge> disc_control;
    std::shared_ptr<LinkControlBridge> link_control;
    std::shared_ptr<ClientHeartbeatPrefs> heartbeat_prefs;
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
