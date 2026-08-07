#pragma once

#include "client/controller_manager.hpp"
#include "host/active_session_slot.hpp"
#include "host/game_catalog.hpp"
#include "host/host_app_config.hpp"
#include "host/host_session_hub.hpp"
#include "host/input_router_demux.hpp"
#include "host/session_types.hpp"
#include "host/streaming_audio_sink.hpp"

#include <atomic>
#include <string>
#include <string_view>
#include <vector>

namespace archstreamer {

/**
 * Owns live session threads (ActiveSessionSlot).
 * Applies Lobby commands after each Lobby update — does not accept TCP.
 */
class SessionManager {
public:
    struct Config {
        HostAppConfig host_config;
        GameCatalog* catalog = nullptr;
        GameList game_list;
        HostSessionHub* hub = nullptr;
        InputRouterDemux* input_demux = nullptr;
        StreamingAudioSink* streaming_audio = nullptr;
        std::optional<ControllerDevice> bridge_device;
        std::function<bool()> should_stop;
        std::uint8_t max_slots = 2;
    };

    explicit SessionManager(Config config);
    ~SessionManager();

    SessionManager(const SessionManager&) = delete;
    SessionManager& operator=(const SessionManager&) = delete;

    /** Apply Lobby output (StartSession / EnqueueJoin / Destroy / …). */
    void apply_commands(std::vector<LobbyCommand> commands);

    /** Join and drop finished slots; returns how many were reaped. */
    std::size_t reap_finished();

    void request_stop_all();
    void join_all();

    std::size_t live_count() const;
    std::vector<SessionStatusSnapshot> statuses() const;

    ActiveSessionSlot* find_by_session_id(const SessionId& session_id);
    const ActiveSessionSlot* find_by_session_id(const SessionId& session_id) const;

    /** Allocate a new session id before starting a slot. */
    static SessionId make_session_id();

private:
    void apply_one(LobbyCommand& command);
    void start_session(SessionPlan plan, SessionId session_id);
    void destroy_session(const SessionId& session_id, std::string_view reason);

    Config config_;
    mutable std::mutex mutex_;
    std::vector<std::unique_ptr<ActiveSessionSlot>> slots_;
};

} // namespace archstreamer
