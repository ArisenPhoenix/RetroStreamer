#pragma once

#include "common/protocol.hpp"
#include "host/session_lobby.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace archstreamer {

/** Stable id for a live play session (Lobby / SessionManager / links / diagnostics). */
using SessionId = std::string;

enum class SessionPhase : std::uint8_t {
    Starting = 0,
    Running,
    PausedReconnect,
    Stopping,
    Finished,
};

/** Read-only snapshot for Lobby / UI / debugging. */
struct SessionStatusSnapshot {
    SessionId session_id;
    int slot_index = -1;
    GameSessionMode mode = GameSessionMode::SinglePlayer;
    SessionPhase phase = SessionPhase::Starting;
    GameId game_id;
    std::string save_username;
    std::uint8_t seated_players = 0;
    std::uint8_t connected_clients = 0;
    bool finished = false;
    /** Session asked the orchestrator to tear it down (optional self-kill path). */
    std::optional<std::string> request_destroy_reason;
};

enum class LobbyClientBucket : std::uint8_t {
    /** Catalog / presence — TCP up, not in a play session. */
    Connected = 0,
    /** Gathering for Multiplayer, or disconnected-from-MP on reconnect timer. */
    Multiplayer,
    /** Associated with a SinglePlayer session (including reconnect hold). */
    SinglePlayerSession,
};

struct LobbyClientSnapshot {
    ClientId client_id = 0;
    std::string username;
    LobbyClientBucket bucket = LobbyClientBucket::Connected;
    std::optional<SessionId> session_id;
    std::optional<std::chrono::steady_clock::time_point> reconnect_deadline;
};

struct LobbyStatusSnapshot {
    std::vector<LobbyClientSnapshot> connected;
    std::vector<LobbyClientSnapshot> multiplayer;
    std::vector<LobbyClientSnapshot> singleplayer_sessions;
    std::vector<SessionStatusSnapshot> sessions;
    std::size_t active_link_count = 0;
};

/**
 * Commands Lobby emits for SessionManager / HostApp to apply.
 * Lobby never starts emulator threads itself — it only decides membership.
 */
struct LobbyCommand {
    enum class Kind : std::uint8_t {
        StartSession = 1,
        EnqueueJoin = 2,
        DestroySession = 3,
        PauseForReconnect = 4,
        ResumeSession = 5,
    };

    Kind kind = Kind::StartSession;
    SessionId session_id;
    std::string reason;
    std::optional<SessionPlan> plan;
    std::optional<ClientHello> hello;
    std::optional<TcpStream> stream;
    bool is_reconnect = false;
};

inline const char* session_phase_name(SessionPhase phase) {
    switch (phase) {
    case SessionPhase::Starting:
        return "starting";
    case SessionPhase::Running:
        return "running";
    case SessionPhase::PausedReconnect:
        return "paused_reconnect";
    case SessionPhase::Stopping:
        return "stopping";
    case SessionPhase::Finished:
        return "finished";
    }
    return "unknown";
}

} // namespace archstreamer
