#pragma once

#include "host/host_app_config.hpp"
#include "host/host_session_hub.hpp"
#include "host/input_router.hpp"
#include "host/input_router_demux.hpp"
#include "host/launch_environment.hpp"
#include "host/media_server.hpp"
#include "host/network_input_receiver.hpp"
#include "host/save_profile.hpp"
#include "host/session_control_monitor.hpp"
#include "host/session_launch_assemble.hpp"
#include "host/session_lobby.hpp"
#include "host/session_runtime.hpp"
#include "host/session_slot_lease.hpp"
#include "host/streaming_audio_sink.hpp"
#include "host/virtual_keyboard.hpp"
#include "host/platform/default_host_platform.hpp"
#include "client/controller_manager.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <thread>
#include <utility>

namespace archstreamer {

class GameCatalog;
struct LocalControllerBridge;
class ControllerDevice;
struct GpuDevice;

struct ActiveSessionSlotConfig {
    int slot_index = 0;
    /** Holds slot_index against every other host process for the slot's lifetime. */
    SessionSlotLease slot_lease;
    HostAppConfig host_config;
    SessionPlan plan;
    HostLaunchPlan launch_plan;
    GameCatalog* catalog = nullptr;
    GameList game_list;
    HostSessionHub* hub = nullptr;
    InputRouterDemux* input_demux = nullptr;
    StreamingAudioSink* streaming_audio = nullptr;
    std::optional<ControllerDevice> bridge_device;
    std::function<bool()> should_stop;
};

/**
 * One live play session: own emulator, capture/media, pads, and control clients.
 * Runs on a dedicated thread; HostApp accepts TCP and routes joins / new SP slots.
 */
class ActiveSessionSlot {
public:
    explicit ActiveSessionSlot(ActiveSessionSlotConfig config);
    ~ActiveSessionSlot();

    ActiveSessionSlot(const ActiveSessionSlot&) = delete;
    ActiveSessionSlot& operator=(const ActiveSessionSlot&) = delete;

    void start();
    void request_stop();
    bool finished() const { return finished_.load(); }
    void join();

    int slot_index() const { return config_.slot_index; }
    SessionPlan& plan() { return config_.plan; }
    const SessionPlan& plan() const { return config_.plan; }
    bool is_multiplayer() const {
        return config_.plan.session_mode == GameSessionMode::Multiplayer;
    }

    /** Queue an already-handshaken late viewer / reconnect stream for this slot. */
    void enqueue_join(TcpStream stream, ClientHello hello, bool is_reconnect);

    /** After GBA Link match: relaunch this slot's RetroArch as netplay host or client. */
    void request_gba_netplay_relaunch(GbaNetplayRelaunchRequest request);

    MediaServer* media_server() { return media_server_.get(); }
    InputRouter* input_router() { return input_router_.get(); }
    std::size_t& media_index() { return media_index_; }

private:
    struct PendingJoin {
        TcpStream stream;
        ClientHello hello;
        bool is_reconnect = false;
    };

    void thread_main();
    void run_session();
    void drain_pending_joins();
    void register_input_clients();
    void unregister_input_clients();
    void shutdown_media_and_clients(const std::string& end_reason);
    std::optional<GbaNetplayRelaunchRequest> consume_gba_netplay_relaunch();

    /** Locals needed by mid-session RetroArch relaunches (GB link / GBA netplay). */
    struct RelaunchContext {
        bool capture_fullscreen = false;
        const std::optional<GpuDevice>* resolved_gpu = nullptr;
        EmulatorLaunchEnvRequest* launch_env_request = nullptr;
    };

    RetroArchOverrideParams make_relaunch_override_params(
        RetroArchPort players,
        const std::filesystem::path& core_path,
        const RelaunchContext& ctx) const;

    /** nullopt = keep running; set = end session with this reason. */
    std::optional<std::string> poll_session_monitor_stop();
    std::optional<std::string> handle_pending_link_promotion();
    std::optional<std::string> handle_gb_link_relaunch(const RelaunchContext& ctx);
    std::optional<std::string> handle_gba_netplay_relaunch(const RelaunchContext& ctx);

    ActiveSessionSlotConfig config_;
    std::thread worker_;
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> finished_{false};

    std::mutex join_mutex_;
    std::queue<PendingJoin> pending_joins_;

    std::mutex gba_netplay_mutex_;
    std::optional<GbaNetplayRelaunchRequest> pending_gba_netplay_;

    std::unique_ptr<HostVirtualGamepadBus> gamepads_;
    std::unique_ptr<VirtualKeyboard> keyboard_;
    std::unique_ptr<InputRouter> input_router_;
    std::unique_ptr<MediaServer> media_server_;
    std::unique_ptr<SessionRuntime> session_runtime_;
    std::optional<SessionControlMonitor> session_monitor_;
    SaveProfile save_profile_;
    std::string system_key_;
    std::size_t media_index_ = 0;
    std::size_t virtual_joypad_index_ = 0;
    bool gamescope_capture_ = false;
    bool use_virtual_capture_ = false;
    HostAppConfig slot_config_;
};

/** Clamp host max concurrent SP slots (also multi lobby size). */
inline std::uint8_t clamp_max_session_slots(std::uint8_t clients) {
    if (clients < 2) {
        return 2;
    }
    if (clients > 4) {
        return 4;
    }
    return clients;
}

/** Apply per-slot display / media / netcmd offsets onto a config copy. */
HostAppConfig slot_adjusted_config(HostAppConfig config, int slot_index);

/** Display number out of a `:N` string; 99 when unparseable. */
int parse_virtual_display_number(const std::string& virtual_display);

/** Offset virtual pad product ids so concurrent slots do not collide. */
void apply_slot_product_id_offset(std::vector<VirtualGamepadIdentity>& identities, int slot_index);

} // namespace archstreamer
