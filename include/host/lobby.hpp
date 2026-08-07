#pragma once

#include "client/controller_manager.hpp"
#include "host/game_catalog.hpp"
#include "host/host_app_config.hpp"
#include "host/host_session_hub.hpp"
#include "host/input_router_demux.hpp"
#include "host/network_input_receiver.hpp"
#include "host/session_manager.hpp"
#include "host/session_types.hpp"
#include "host/streaming_audio_sink.hpp"
#include "common/platform/default_platform.hpp"
#include "common/protocol.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace archstreamer {

/**
 * Persistent admission / staging surface for the control port.
 *
 * Buckets:
 *  - connected: catalog presence
 *  - multiplayer: MP gather / reconnect-hold players
 *  - singleplayer session refs: tracked via SessionManager statuses
 *
 * Links (matchmaking + cable allocation) live here via HostSessionHub so they
 * are not tied to a single SessionPlan.
 *
 * Control order: Lobby::update (or lobby thread) emits commands; HostApp applies
 * them through SessionManager before relying on session state.
 */
class Lobby {
public:
    struct Config {
        HostAppConfig host_config;
        GameCatalog* catalog = nullptr;
        GameList game_list;
        StreamingAudioSink* streaming_audio = nullptr;
        std::optional<ControllerDevice> bridge_device;
        std::function<bool()> should_stop;
    };

    explicit Lobby(Config config);
    ~Lobby();

    Lobby(const Lobby&) = delete;
    Lobby& operator=(const Lobby&) = delete;

    HostSessionHub& hub() { return hub_; }
    const HostSessionHub& hub() const { return hub_; }
    InputRouterDemux& input_demux() { return demux_; }
    SessionManager& sessions() { return *session_manager_; }
    const SessionManager& sessions() const { return *session_manager_; }

    /** Start UDP input + spawn the lobby accept thread. */
    void start();
    void request_stop();
    void join();

    /**
     * HostApp control tick: drain Lobby commands into SessionManager, then reap.
     * Safe to call from the process main thread while Lobby runs on its thread.
     */
    void apply_to_sessions();

    /** Blocking run: start, loop apply_to_sessions until stop, then shutdown. */
    int run_until_stop();

    LobbyStatusSnapshot status_snapshot() const;
    std::vector<LobbyCommand> drain_commands();

private:
    struct ConnectedClient {
        ClientId client_id = 0;
        std::string username;
        TcpStream stream;
    };

    struct MultiplayerClient {
        ClientId client_id = 0;
        std::string username;
        ClientHello hello;
        /** Empty while still holding the TCP stream in-lobby (gather / reconnect). */
        std::optional<TcpStream> stream;
        std::optional<SessionId> session_id;
        std::optional<std::chrono::steady_clock::time_point> reconnect_deadline;
    };

    void thread_main();
    void prepare_runtime();
    void shutdown_runtime();
    void poll_connected_bucket();
    void accept_once();
    void handle_presence(TcpStream stream, LobbyPresence presence);
    void handle_hello(TcpStream stream, ClientHello hello);
    void enqueue_command(LobbyCommand command);
    void publish_connected(const ConnectedClient& client) const;
    void erase_connected_at(std::size_t index);
    std::uint8_t max_slots() const;

    Config config_;
    HostSessionHub hub_;
    InputRouterDemux demux_;
    std::unique_ptr<NetworkInputReceiver> network_receiver_;
    std::unique_ptr<TcpListener> listener_;
    std::unique_ptr<SessionManager> session_manager_;

    mutable std::mutex mutex_;
    std::vector<ConnectedClient> connected_;
    std::vector<MultiplayerClient> multiplayer_;
    std::vector<LobbyCommand> pending_commands_;

    std::thread worker_;
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> started_{false};
    std::filesystem::path art_root_;
};

} // namespace archstreamer
