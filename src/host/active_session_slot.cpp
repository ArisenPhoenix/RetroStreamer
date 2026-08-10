#include "host/active_session_slot.hpp"

#include "common/cli_common.hpp"
#include "common/ds_touch_mapping.hpp"
#include "common/participant_role.hpp"
#include "common/serialization.hpp"
#include "client/controller_backend.hpp"
#include "host/cadence_session_events.hpp"
#include "host/cadence_session_tracker.hpp"
#include "host/capture_platform.hpp"
#include "host/client_stream_policy.hpp"
#include "host/game_catalog.hpp"
#include "host/gpu_select.hpp"
#include "host/host_launch_planner.hpp"
#include "host/host_session_helpers.hpp"
#include "host/launch_environment.hpp"
#include "host/link_cable_backend.hpp"
#include "host/local_controller_bridge.hpp"
#include "host/retroarch_config_writer.hpp"
#include "host/retroarch_netcmd.hpp"
#include "host/retroarch_resolve.hpp"
#include "host/save_active_sessions.hpp"
#include "host/game_meta_store.hpp"
#include "host/session_lobby.hpp"
#include "host/session_launch_assemble.hpp"
#include "host/session_run_helpers.hpp"
#include "host/session_audio_channel.hpp"
#include "host/standalone_emulator.hpp"
#include "host/session_launch_types.hpp"
#include "host/switch_save_share.hpp"
#include "host/switch/switch_backend.hpp"
#include "host/switch/ryujinx_controls.hpp"
#include "host/nds/melonds_backend.hpp"
#include "host/virtual_joypad_resolve.hpp"
#include "host/pad_plan.hpp"
#include "host/virtual_display.hpp"
#include "host/virtual_keyboard.hpp"
#include "host/soft_keyboard_host.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <thread>
#include <utility>

namespace archstreamer {
namespace {

bool should_use_slot_streaming_sink(const std::string& audio_source) {
    if (audio_source.empty()) {
        return true;
    }
    if (!audio_source.ends_with(".monitor")) {
        return false;
    }
    const auto sink = audio_source.substr(0, audio_source.size() - 8);
    return StreamingAudioSink::is_streaming_sink_name(sink);
}

void send_ds_screen_layout_to_client(
    SessionClientConnection& client,
    const DsScreenLayout& layout) {
    if (client.connection_state != SessionConnectionState::Connected) {
        return;
    }
    try {
        client.stream.send_packet(serialize_packet(layout));
    } catch (const std::exception& error) {
        std::cerr
            << "Failed to send DsScreenLayout to client "
            << static_cast<int>(client.client_id) << ": " << error.what() << '\n';
    }
}

void broadcast_ds_screen_layout(SessionPlan& plan, const DsScreenLayout& layout) {
    for (auto& client : plan.clients) {
        send_ds_screen_layout_to_client(client, layout);
    }
}

} // namespace

int parse_virtual_display_number(const std::string& virtual_display) {
    if (virtual_display.empty() || virtual_display.front() != ':') {
        return 99;
    }
    try {
        return std::stoi(virtual_display.substr(1));
    } catch (const std::exception&) {
        return 99;
    }
}

HostAppConfig slot_adjusted_config(HostAppConfig config, int slot_index) {
    const int display_num = parse_virtual_display_number(config.virtual_display);
    config.virtual_display = ":" + std::to_string(display_num + slot_index);
    config.video_port = static_cast<std::uint16_t>(config.video_port + slot_index * 32);
    config.audio_port = static_cast<std::uint16_t>(config.audio_port + slot_index * 32);
    return config;
}

void apply_slot_product_id_offset(std::vector<VirtualGamepadIdentity>& identities, int slot_index) {
    const auto offset = static_cast<std::uint16_t>(slot_index * 8);
    for (auto& identity : identities) {
        identity.product_id = static_cast<std::uint16_t>(identity.product_id + offset);
    }
}

struct ActiveSaveSessionGuard {
    std::filesystem::path root;
    int slot = -1;

    ~ActiveSaveSessionGuard() {
        if (slot >= 0 && !root.empty()) {
            clear_active_save_session(root, slot);
        }
    }
};

struct SlotLaunchEnvironment {
    CapturePlan capture;
    EmulatorLaunchEnvRequest request;
    std::optional<GpuDevice> resolved_gpu;
    std::string gamescope_vk_device;
    std::string xtest_display;
    int nvenc_cuda_device_id = -1;
};

SlotLaunchEnvironment prepare_slot_launch_environment(
    HostAppConfig& config,
    RetroArchLaunchConfig& launch_config,
    int slot,
    bool host_plays_locally,
    const SessionId& session_id) {
    SlotLaunchEnvironment env;
    env.capture = resolve_capture_plan(config, launch_config);
    env.request.stream_media = config.audio || config.video;
    env.request.stream_audio = config.audio;
    env.request.host_plays_locally = host_plays_locally;
    env.request.audio_source = config.audio_source;
    env.request.ignore_devices = *config.ignore_controller;
    env.request.use_virtual_capture = env.capture.use_virtual_capture;
    env.request.gamescope_capture = env.capture.gamescope_capture;
    env.request.virtualgl_capture = env.capture.virtualgl_capture;
    env.request.capture_display = env.capture.capture_display;
    env.xtest_display = env.capture.gamescope_capture
        ? gamescope_xtest_display_for_slot(slot)
        : env.capture.capture_display;
    if (env.capture.gamescope_capture) {
        env.request.xtest_display = env.xtest_display;
    }
    if (!session_id.empty()) {
        env.request.session_id = session_id;
        if (env.capture.gamescope_capture || env.capture.use_virtual_capture) {
            register_session_xtest_display(session_id, env.xtest_display);
            std::cout
                << "session slot " << slot << ": session " << session_id
                << " XTest lease " << env.xtest_display << '\n';
        }
    }

    auto resolved_encode = resolve_render_gpu(config.encode_gpu);
    env.resolved_gpu = resolve_render_gpu(effective_render_gpu_selection(config));
    if (resolved_encode.has_value() && resolved_encode->nvidia_index >= 0) {
        env.nvenc_cuda_device_id = resolved_encode->nvidia_index;
    }
    if (env.resolved_gpu.has_value()) {
        if (resolved_encode.has_value() && resolved_encode->id == env.resolved_gpu->id) {
            std::cout
                << "session slot " << slot << ": GPU " << env.resolved_gpu->name
                << " [" << env.resolved_gpu->id << "] (encode+render)\n";
        } else {
            if (resolved_encode.has_value()) {
                std::cout
                    << "session slot " << slot << ": encode GPU " << resolved_encode->name
                    << " [" << resolved_encode->id << "]\n";
            }
            std::cout
                << "session slot " << slot << ": render GPU " << env.resolved_gpu->name
                << " [" << env.resolved_gpu->id << "]\n";
        }
        if (const auto vd = pci_vendor_device_id(env.resolved_gpu->pci_bus); vd.has_value()) {
            env.gamescope_vk_device = *vd;
        }
        env.request.render_gpu = *env.resolved_gpu;
    } else if (resolved_encode.has_value()) {
        std::cout
            << "session slot " << slot << ": encode GPU " << resolved_encode->name
            << " [" << resolved_encode->id << "]\n";
    }
    if (env.gamescope_vk_device.empty()) {
        env.gamescope_vk_device = "10de:2504";
    }
    return env;
}

SessionMediaPlan prepare_slot_media_plan(HostAppConfig& config, SessionPlan& plan) {
    SessionMediaPlan media;
    parse_video_resolution(config.video_resolution, media.capture_w, media.capture_h);
    configure_initial_session_video(plan, media.capture_w, media.capture_h);
    media.config = media_plan_config_for(config);
    media.config.initial_video_settings = plan.session_video_settings;
    if (config.video || config.audio) {
        media.destinations = media_destinations_for_session(media.config, plan);
        media.streams = media_streams_for_dry_run(media.config, media.destinations);
    }
    return media;
}

SessionDevicePlan resolve_slot_device_plan(
    const HostAppConfig& config,
    const HostLaunchPlan& launch_plan,
    int slot) {
    SessionDevicePlan selection;
    selection.product_id_base = static_cast<std::uint16_t>(0xa517 + slot * 8);
    const bool use_udev = config.retroarch_joypad_driver == "udev";
    // Concurrent slots must not see each other's ArchStreamer uinput pads. Exclusive
    // PadPlan IGNORES sibling VID/PIDs (EXCEPT alone fails for uinput under Ryujinx).
    selection.shared_pad_plan = resolve_retroarch_slot_pad_plan(
        launch_plan.players,
        config.ignore_controller.value_or(""),
        config.verbose,
        selection.product_id_base,
        use_udev);
    if (use_udev) {
        selection.resolved_indices = selection.shared_pad_plan.udev_indices;
        selection.resolved_pads = selection.shared_pad_plan.pads;
    } else {
        selection.resolved_pads = selection.shared_pad_plan.pads;
        selection.resolved_indices.reserve(selection.resolved_pads.size());
        for (const auto& pad : selection.resolved_pads) {
            selection.resolved_indices.push_back(pad.sdl_index);
        }
    }
    if (config.virtual_joypad_index.has_value()) {
        selection.virtual_joypad_index = *config.virtual_joypad_index;
    } else if (!selection.resolved_indices.empty()) {
        selection.virtual_joypad_index = selection.resolved_indices.front();
    }
    return selection;
}

std::unique_ptr<MelonDsCtrlClient> configure_slot_input_router(
    InputRouter& input_router,
    VirtualKeyboard& keyboard,
    const HostLaunchPlan& launch_plan,
    const SessionBackendState& backends) {
    input_router.set_seat_assignment(launch_plan.seats);
    if (backends.switch_backend) {
        input_router.set_emulator_backend(EmulatorControlBackend::Ryujinx);
    } else if (backends.melonds_backend != nullptr) {
        input_router.set_emulator_backend(EmulatorControlBackend::MelonDS);
    } else {
        input_router.set_emulator_backend(EmulatorControlBackend::RetroArch);
    }

    auto melonds_touch_ctrl = std::unique_ptr<MelonDsCtrlClient>{};
    if (backends.melonds_backend != nullptr && backends.melonds_backend->profile() != nullptr) {
        const auto& ctrl_name = backends.melonds_backend->profile()->ctrl_server_name;
        keyboard.set_melonds_ctrl_name(ctrl_name);
        melonds_touch_ctrl = std::make_unique<MelonDsCtrlClient>(ctrl_name);
        MelonDsCtrlClient* touch_ctrl = melonds_touch_ctrl.get();
        input_router.set_touch_handler([touch_ctrl](const TouchInput& input) {
            if (touch_ctrl == nullptr) {
                return false;
            }
            if (input.pressed) {
                std::uint16_t x = 0;
                std::uint16_t y = 0;
                ds_coords_from_normalized_u16(input.x, input.y, x, y);
                return touch_ctrl->touch(x, y);
            }
            return touch_ctrl->touch_end();
        });
    }
    return melonds_touch_ctrl;
}

ActiveSessionSlot::ActiveSessionSlot(ActiveSessionSlotConfig config)
    : config_(std::move(config))
    , slot_config_(slot_adjusted_config(config_.host_config, config_.slot_index)) {
}

ActiveSessionSlot::~ActiveSessionSlot() {
    request_stop();
    join();
}

void ActiveSessionSlot::start() {
    if (config_.hub != nullptr) {
        config_.hub->register_slot(this);
    }
    worker_ = std::thread([this] { thread_main(); });
}

void ActiveSessionSlot::request_stop() {
    stop_requested_.store(true);
}

void ActiveSessionSlot::request_destroy(std::string reason) {
    std::lock_guard lock(destroy_mutex_);
    if (!request_destroy_reason_.has_value()) {
        request_destroy_reason_ = std::move(reason);
    }
    stop_requested_.store(true);
}

SessionStatusSnapshot ActiveSessionSlot::status_snapshot() const {
    SessionStatusSnapshot snap;
    snap.session_id = config_.session_id;
    snap.slot_index = config_.slot_index;
    snap.mode = config_.plan.session_mode;
    snap.game_id = config_.plan.selected_game_id;
    snap.save_username = config_.plan.save_username;
    snap.finished = finished_.load();
    snap.phase = snap.finished ? SessionPhase::Finished
        : (stop_requested_.load() ? SessionPhase::Stopping : SessionPhase::Running);
    snap.seated_players = static_cast<std::uint8_t>(assigned_player_count(config_.plan.seats));
    std::uint8_t connected = 0;
    for (const auto& client : config_.plan.clients) {
        if (client.connection_state == SessionConnectionState::Connected) {
            ++connected;
        }
    }
    snap.connected_clients = connected;
    {
        std::lock_guard lock(destroy_mutex_);
        snap.request_destroy_reason = request_destroy_reason_;
    }
    return snap;
}

void ActiveSessionSlot::join() {
    if (worker_.joinable()) {
        worker_.join();
    }
}

void ActiveSessionSlot::enqueue_join(TcpStream stream, ClientHello hello, bool is_reconnect) {
    std::lock_guard lock(join_mutex_);
    pending_joins_.push(PendingJoin{std::move(stream), std::move(hello), is_reconnect});
}

void ActiveSessionSlot::request_gba_netplay_relaunch(GbaNetplayRelaunchRequest request) {
    std::lock_guard lock(gba_netplay_mutex_);
    pending_gba_netplay_ = std::move(request);
}

std::optional<GbaNetplayRelaunchRequest> ActiveSessionSlot::consume_gba_netplay_relaunch() {
    std::lock_guard lock(gba_netplay_mutex_);
    if (!pending_gba_netplay_.has_value()) {
        return std::nullopt;
    }
    auto request = std::move(*pending_gba_netplay_);
    pending_gba_netplay_.reset();
    return request;
}

RetroArchOverrideParams ActiveSessionSlot::make_relaunch_override_params(
    RetroArchPort players,
    const std::filesystem::path& core_path,
    const RelaunchContext& ctx) const {
    RetroArchOverrideParams override_params;
    override_params.first_virtual_joypad_index = virtual_joypad_index_;
    override_params.identities = &config_.launch_plan.virtual_identities;
    override_params.joypad_driver = slot_config_.retroarch_joypad_driver;
    override_params.players = players;
    override_params.save_profile = &save_profile_;
    override_params.realtime_pacing = slot_config_.audio || slot_config_.video;
    override_params.capture_fullscreen = ctx.capture_fullscreen && use_virtual_capture_;
    override_params.capture_resolution = slot_config_.video_resolution;
    const bool have_gpu = ctx.resolved_gpu != nullptr && ctx.resolved_gpu->has_value();
    override_params.vulkan_gpu_index =
        (!use_virtual_capture_ && have_gpu) ? (*ctx.resolved_gpu)->vulkan_index : -1;
    override_params.system_key = system_key_;
    override_params.core_path = core_path;
    override_params.resolution_scale = slot_config_.resolution.retroarch_scale;
    override_params.slot_index = config_.slot_index;
    override_params.network_cmd_port = config_.plan.retroarch_netcmd_port;
    {
        std::vector<ClientHello> hellos;
        hellos.reserve(config_.plan.clients.size());
        for (const auto& client : config_.plan.clients) {
            hellos.push_back(client.hello);
        }
        override_params.display_layout =
            resolve_display_layout_preference(config_.plan.host_hello, hellos);
    }
    return override_params;
}

std::optional<std::string> ActiveSessionSlot::poll_session_monitor_stop() {
    if (!session_monitor_.has_value()) {
        return std::nullopt;
    }
    if (const auto reason = session_monitor_->poll(); reason.has_value()) {
        std::cerr
            << "session slot " << config_.slot_index << ": stopping: " << *reason << '\n';
        return reason;
    }
    return std::nullopt;
}

std::optional<std::string> ActiveSessionSlot::handle_pending_link_promotion() {
    auto& plan = config_.plan;
    if (!plan.pending_link_promotion) {
        return std::nullopt;
    }
    plan.pending_link_promotion = false;
    LinkPromotionRequest promotion;
    promotion.logical_host_client_id = plan.pending_link_host_client_id;
    promotion.logical_client_client_id = plan.pending_link_client_client_id;
    promotion.logical_host_username = plan.pending_link_host_username;
    promotion.logical_client_username = plan.pending_link_client_username;
    promotion.system_key = plan.system_key;

    auto link_runtime = promote_to_link_runtime(std::move(session_runtime_), std::move(promotion));
    if (!link_runtime) {
        return "link promotion failed";
    }
    std::cout
        << "session slot " << config_.slot_index << ": link runtime "
        << link_runtime->kind_name() << '\n';
    send_retroarch_netcmd(
        "SHOW_MSG Link runtime: peer instance pending",
        plan.retroarch_netcmd_port);
    session_runtime_ = std::move(link_runtime);
    return std::nullopt;
}

std::optional<std::string> ActiveSessionSlot::handle_gb_link_relaunch(const RelaunchContext& ctx) {
    auto& plan = config_.plan;
    if (session_runtime_ == nullptr ||
        session_runtime_->launch_config().standalone ||
        !plan.link_cable.consume_relaunch_request()) {
        return std::nullopt;
    }
#if defined(ARCHSTREAMER_DEBUG_GB_LINK)
    const auto link_core = plan.link_cable.pending_core_path();
    if (!link_core.has_value()) {
        return std::nullopt;
    }
    if (ctx.launch_env_request == nullptr) {
        return "link cable relaunch failed (missing launch env)";
    }
    session_runtime_->stop_emulator();
    auto& relaunch_config = session_runtime_->launch_config();
    relaunch_config.core_path = *link_core;
    LinkCableBackend::write_dual_gb_core_options();
    apply_retroarch_override_and_env(
        relaunch_config,
        make_relaunch_override_params(
            std::max<RetroArchPort>(config_.launch_plan.players, 2),
            relaunch_config.core_path,
            ctx),
        *ctx.launch_env_request);
    session_runtime_->start_emulator();
    if (!wait_emulator_running(*session_runtime_)) {
        return "link cable relaunch failed (RetroArch exited immediately)";
    }
    send_retroarch_netcmd(
        "SHOW_MSG Link cable active — Cable Club",
        plan.retroarch_netcmd_port);
#else
    (void)ctx;
    std::cerr
        << "session slot " << config_.slot_index
        << ": link cable relaunch ignored (debug off)\n";
#endif
    return std::nullopt;
}

std::optional<std::string> ActiveSessionSlot::handle_gba_netplay_relaunch(const RelaunchContext& ctx) {
    const auto gba = consume_gba_netplay_relaunch();
    if (!gba.has_value()) {
        return std::nullopt;
    }
    if (session_runtime_ == nullptr || session_runtime_->launch_config().standalone) {
        std::cerr
            << "session slot " << config_.slot_index
            << ": GBA netplay ignored (standalone emulator)\n";
        return std::nullopt;
    }
    if (ctx.launch_env_request == nullptr) {
        return "GBA netplay relaunch failed (missing launch env)";
    }

    // Client connects after host has time to bind --host.
    if (!gba->is_host) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2000));
    }
    std::cout
        << "session slot " << config_.slot_index << ": GBA netplay relaunch as "
        << (gba->is_host ? "host" : "client")
        << " port=" << gba->port
        << " core=" << gba->core_path << '\n';
    session_runtime_->stop_emulator();
    auto& relaunch_config = session_runtime_->launch_config();
    if (!gba->core_path.empty()) {
        relaunch_config.core_path = gba->core_path;
    }
    LinkCableBackend::apply_netplay_launch_args(
        relaunch_config.extra_args,
        gba->is_host,
        gba->port,
        gba->nick);
    apply_retroarch_override_and_env(
        relaunch_config,
        make_relaunch_override_params(
            config_.launch_plan.players,
            relaunch_config.core_path,
            ctx),
        *ctx.launch_env_request);
    session_runtime_->start_emulator();
    if (!wait_emulator_running(*session_runtime_, 20)) {
        return "GBA netplay relaunch failed (RetroArch exited immediately)";
    }
    send_retroarch_netcmd(
        gba->is_host
            ? "SHOW_MSG GBA cable host ready — Cable Club"
            : "SHOW_MSG GBA cable connected — Cable Club",
        config_.plan.retroarch_netcmd_port);
    return std::nullopt;
}


void ActiveSessionSlot::thread_main() {
    try {
        run_session();
    } catch (const std::exception& error) {
        std::cerr << "session slot " << config_.slot_index << ": " << error.what() << '\n';
        try {
            send_error_to_session_clients(config_.plan, error.what());
        } catch (const std::exception&) {
        }
        // Always kill the emulator tree on failure. Media-only shutdown left
        // orphan RetroArch/Ryujinx processes holding the next session's pad.
        try {
            if (input_router_ != nullptr) {
                unregister_input_clients();
                input_router_->set_touch_handler({});
                input_router_.reset();
            }
            unplug_session_keyboard(keyboard_.get());
            keyboard_.reset();
            stop_session_runtime(session_runtime_, /*reset=*/true);
            gamepads_.reset();
            audio_channel_.reset();
        } catch (const std::exception& cleanup_error) {
            std::cerr
                << "session slot " << config_.slot_index
                << ": cleanup after error failed: " << cleanup_error.what() << '\n';
        }
        shutdown_media_and_clients("session error");
    }

    if (input_router_ != nullptr && config_.input_demux != nullptr) {
        config_.input_demux->unregister_all_for(input_router_.get());
    }
    if (config_.hub != nullptr) {
        config_.hub->unregister_slot(this);
    }
    finished_.store(true);
}

void ActiveSessionSlot::register_input_clients() {
    if (config_.input_demux == nullptr || input_router_ == nullptr) {
        std::cerr
            << "session slot " << config_.slot_index
            << ": input demux register skipped (demux or router null)\n";
        return;
    }
    if (config_.plan.seats.seats.empty()) {
        std::cerr
            << "session slot " << config_.slot_index
            << ": input demux register skipped (no seats in plan)\n";
        return;
    }
    for (const auto& seat : config_.plan.seats.seats) {
        config_.input_demux->register_router(seat.client_id, input_router_.get());
        std::cout
            << "session slot " << config_.slot_index
            << ": input demux ← client " << static_cast<int>(seat.client_id)
            << " local P" << static_cast<int>(seat.local_player) + 1
            << " → RA P" << static_cast<int>(seat.retroarch_port) + 1 << '\n';
    }
}

void ActiveSessionSlot::unregister_input_clients() {
    if (config_.input_demux == nullptr || input_router_ == nullptr) {
        return;
    }
    config_.input_demux->unregister_all_for(input_router_.get());
}

void ActiveSessionSlot::shutdown_media_and_clients(const std::string& end_reason) {
    if (!config_.session_id.empty()) {
        unregister_session_xtest_display(config_.session_id);
    }
    if (cadence_session_live_) {
        record_session_ended(
            config_.slot_index,
            config_.plan.save_username,
            config_.plan.selected_game_id,
            end_reason,
            cadence_tracker_.session_id());
        cadence_session_live_ = false;
    }
    cadence_tracker_.end(end_reason);
    stop_session_media(media_server_);
    send_session_ended_to_clients(config_.plan, end_reason);
}

void ActiveSessionSlot::drain_pending_joins() {
    std::vector<PendingJoin> joins;
    {
        std::lock_guard lock(join_mutex_);
        while (!pending_joins_.empty()) {
            joins.push_back(std::move(pending_joins_.front()));
            pending_joins_.pop();
        }
    }

    auto& plan = config_.plan;
    for (auto& pending : joins) {
        try {
            auto* reconnected_player = static_cast<SessionClientConnection*>(nullptr);
            auto client_id = next_session_client_id(plan);
            if (pending.is_reconnect || pending.hello.requested_players > 0) {
                reconnected_player = disconnected_player_for_reconnect(plan, pending.hello);
                if (reconnected_player == nullptr && pending.hello.requested_players > 0) {
                    throw std::runtime_error("active sessions only accept late viewers or reconnecting players");
                }
                if (reconnected_player != nullptr) {
                    client_id = reconnected_player->client_id;
                }
            }

            if (media_server_ == nullptr) {
                throw std::runtime_error("media server unavailable for pending join");
            }

            // Same control handshake as a fresh lobby join / poll_active_session_joins:
            // Welcome → Seats → Ready → MediaEndpoint → SessionStarting.
            // Skipping Welcome left Android (and any ClientHello waiter) hanging on an
            // open TCP with no heartbeats until the reconnect seat timed out.
            auto welcome = HostWelcome{};
            welcome.client_id = client_id;
            welcome.max_players_for_client = MaxPlayersPerClient;
            welcome.host_is_player = plan.host_hello.has_value();
            pending.stream.send_packet(serialize_packet(welcome));
            pending.stream.send_packet(serialize_packet(plan.seats));
            pending.stream.send_packet(serialize_packet(SessionReady{
                plan.selected_game_id,
                plan.session_mode,
                static_cast<std::uint8_t>(assigned_player_count(plan.seats)),
            }));

            const auto destination_host = media_destination_host(
                media_plan_config_for(slot_config_),
                pending.stream.peer_address());
            auto endpoint = MediaEndpoint{};
            if (pending.hello.wants_video || pending.hello.wants_audio) {
                endpoint = media_server_->add_client(
                    client_id,
                    destination_host,
                    media_index_,
                    pending.hello.wants_video,
                    pending.hello.wants_audio);
                if (!endpoint.video_uri.empty() || !endpoint.audio_uri.empty()) {
                    ++media_index_;
                    pending.stream.send_packet(serialize_packet(endpoint));
                }
            }

            pending.stream.send_packet(serialize_packet(SessionStarting{
                plan.selected_game_id,
                plan.session_mode,
                static_cast<std::uint8_t>(assigned_player_count(plan.seats)),
            }));

            if (reconnected_player != nullptr) {
                reconnected_player->hello = pending.hello;
                reconnected_player->stream = std::move(pending.stream);
                reconnected_player->connection_state = SessionConnectionState::Connected;
                reconnected_player->last_seen = std::chrono::steady_clock::now();
                reconnected_player->disconnected_at = {};
                reconnected_player->disconnect_reason.clear();
                reconnected_player->applied_tier = plan.session_video_tier;
                reconnected_player->applied_size = plan.session_video_size;
                reconnected_player->applied_feel = plan.session_video_feel;
                reconnected_player->applied_bitrate = plan.session_video_bitrate;
                reconnected_player->adaptive_fps_cap = MediaStreamFps::Auto;
                reconnected_player->applied_fps = plan.session_video_fps;
                reconnected_player->pending_tier.reset();
                reconnected_player->pending_size.reset();
                reconnected_player->pending_feel.reset();
                reconnected_player->pending_bitrate.reset();
                reconnected_player->pending_fps.reset();
                reconnected_player->pending_video_uri.reset();
                reconnected_player->video_cutover_started = {};
                reconnected_player->video_cutover_failures = 0;
                reconnected_player->video_cutover_suppressed = false;
                reconnected_player->positive_video_heartbeats = 0;
                reconnected_player->initial_video_settings_ready = false;
                reconnected_player->initial_video_settings_defer_logged = false;
                // add_client restarts the shared encode; arm stall recovery window.
                reconnected_player->last_video_reconfigure = std::chrono::steady_clock::now();
                reconnected_player->video_zero_frame_streak = 0;
                if (!endpoint.video_uri.empty() || !endpoint.audio_uri.empty()) {
                    reconnected_player->media_endpoint = endpoint;
                } else {
                    reconnected_player->media_endpoint.reset();
                }
                std::cout
                    << "session slot " << config_.slot_index << ": player "
                    << static_cast<int>(client_id)
                    << " reconnected username=" << pending.hello.username << ".\n";
                record_client_joined(
                    config_.slot_index,
                    pending.hello.username,
                    plan.selected_game_id,
                    "reconnect",
                    cadence_tracker_.session_id());
                {
                    ConnectedClientPresence presence;
                    presence.username = pending.hello.username;
                    presence.client_id = client_id;
                    presence.slot_index = config_.slot_index;
                    presence.game_id = plan.selected_game_id;
                    presence.phase = "session";
                    presence.seated = pending.hello.requested_players > 0;
                    publish_connected_client(config_.host_config.save_root, presence);
                }
            } else {
                plan.clients.push_back(SessionClientConnection{
                    client_id,
                    pending.hello,
                    std::move(pending.stream),
                });
                std::cout
                    << "session slot " << config_.slot_index << ": late viewer "
                    << static_cast<int>(client_id)
                    << " joined username=" << pending.hello.username << ".\n";
                record_client_joined(
                    config_.slot_index,
                    pending.hello.username,
                    plan.selected_game_id,
                    pending.hello.requested_players > 0 ? "player" : "viewer",
                    cadence_tracker_.session_id());
                {
                    ConnectedClientPresence presence;
                    presence.username = pending.hello.username;
                    presence.client_id = client_id;
                    presence.slot_index = config_.slot_index;
                    presence.game_id = plan.selected_game_id;
                    presence.phase = "session";
                    presence.seated = pending.hello.requested_players > 0;
                    publish_connected_client(config_.host_config.save_root, presence);
                }
            }

            if (config_.input_demux != nullptr && input_router_ != nullptr) {
                config_.input_demux->register_router(client_id, input_router_.get());
            }

            // Late join / reconnect: push current bottom-screen hit target if known.
            if (last_ds_screen_layout_.has_value()) {
                auto* target = reconnected_player;
                if (target == nullptr && !plan.clients.empty()) {
                    target = &plan.clients.back();
                }
                if (target != nullptr) {
                    send_ds_screen_layout_to_client(*target, *last_ds_screen_layout_);
                }
            }
        } catch (const std::exception& error) {
            try {
                pending.stream.send_packet(serialize_packet(ErrorPacket{error.what()}));
            } catch (const std::exception&) {
            }
            std::cerr
                << "session slot " << config_.slot_index << ": rejected pending join: "
                << error.what() << '\n';
        }
    }
}

struct SlotContent {
    std::string active_display_name;
    std::filesystem::path catalog_content_path;
    std::string m3m_title_id;
};

struct SlotBackendPrepareRequest {
    int slot = 0;
    HostAppConfig& config;
    SessionPlan& plan;
    HostLaunchPlan& launch_plan;
    RetroArchLaunchConfig& launch_config;
    SaveProfile& save_profile;
    const SlotContent& content;
    const std::string& system_key;
    bool use_virtual_capture = false;
    bool gamescope_capture = false;
    VirtualKeyboard* keyboard = nullptr;
    SlotLaunchEnvironment& launch_env;
    SessionDevicePlan& devices;
    SessionBackendState& backends;
};

void prepare_slot_backend(const SlotBackendPrepareRequest& req) {
    if (req.system_key == "switch") {
        const auto runtime = resolve_switch_runtime();
        if (!runtime.has_value()) {
            const auto message = switch_runtime_unavailable_message();
            send_error_to_session_clients(req.plan, message);
            throw std::runtime_error(message);
        }
        req.launch_config.standalone = true;
        req.launch_config.core_path = runtime->path;
        req.launch_config.standalone_args_before_content = runtime->args_before_content;
        req.backends.switch_backend = make_switch_backend(*runtime);
    } else if (req.system_key == "nds" && melonds_runtime_available()) {
        const auto runtime = resolve_melonds_runtime();
        if (!runtime.has_value()) {
            const auto message = melonds_unavailable_message();
            send_error_to_session_clients(req.plan, message);
            throw std::runtime_error(message);
        }
        req.launch_config.standalone = true;
        req.launch_config.core_path = runtime->path;
        req.launch_config.standalone_args_before_content = runtime->args_before_content;
        req.backends.melonds_backend = make_melonds_backend();
    } else if (req.launch_config.standalone) {
        const auto message =
            "standalone launch requested for unsupported system_key=" + req.system_key;
        send_error_to_session_clients(req.plan, message);
        throw std::runtime_error(message);
    }

    if (req.backends.switch_backend && req.keyboard != nullptr) {
        req.keyboard->set_switch_style_hotkeys(true);
    }

    if (req.backends.switch_backend) {
        std::vector<ClientHello> client_hellos;
        client_hellos.reserve(req.plan.clients.size());
        for (const auto& client : req.plan.clients) {
            client_hellos.push_back(client.hello);
        }
        const auto prefer_handheld_mode = session_prefers_switch_handheld_mode(req.plan);
        const auto profile_name = resolve_switch_profile_display_name(
            req.save_profile.username, req.plan.host_hello, client_hellos);
        const auto switch_content_stem = !req.content.catalog_content_path.empty()
            ? req.content.catalog_content_path.stem().string()
            : req.launch_config.content_path.stem().string();
        auto switch_title_id = req.content.m3m_title_id;
        if (switch_title_id.empty()) {
            switch_title_id = resolve_switch_title_id_for_catalog(
                req.save_profile,
                switch_content_stem,
                req.launch_config.content_path);
        }
        auto switch_prep = req.backends.switch_backend->prepare(
            req.launch_config,
            SwitchBackendPrepContext{
                req.save_profile,
                req.launch_plan.players,
                req.config.verbose,
                req.devices.product_id_base,
                req.config.ignore_controller.value_or(""),
                req.config.graphics_api,
                req.launch_env.capture.virtualgl_capture,
                req.gamescope_capture,
                req.config.resolution.switch_scale,
                prefer_handheld_mode,
                &req.launch_env.resolved_gpu,
                profile_name,
                std::move(req.devices.resolved_pads),
                static_cast<std::size_t>(std::max(0, req.slot)),
                req.launch_plan.game_id,
                switch_content_stem,
                switch_title_id,
            });
        req.devices.resolved_pads = std::move(switch_prep.resolved_pads);
        req.backends.switch_backend->assign_launch_env_profile(req.launch_env.request, switch_prep);
        log_switch_backend_prep(
            *req.backends.switch_backend,
            req.launch_env.request,
            switch_prep,
            req.config.resolution.switch_scale,
            req.launch_env.resolved_gpu,
            req.slot);
        req.backends.switch_launch_content_stem = switch_content_stem;
        req.backends.switch_launch_title_id = switch_title_id;
        if (req.backends.switch_backend->enable_soft_keyboard()) {
            if (!req.plan.soft_keyboard) {
                req.plan.soft_keyboard = std::make_shared<SoftKeyboardHostBridge>();
            }
            req.devices.soft_keyboard_fallback = profile_name;
            req.devices.arm_soft_keyboard = true;
        }
    } else if (req.backends.melonds_backend) {
        std::vector<ClientHello> client_hellos;
        client_hellos.reserve(req.plan.clients.size());
        for (const auto& client : req.plan.clients) {
            client_hellos.push_back(client.hello);
        }
        const auto profile_name = resolve_switch_profile_display_name(
            req.save_profile.username,
            req.plan.host_hello,
            client_hellos);
        const auto nds_layout =
            resolve_display_layout_preference(req.plan.host_hello, client_hellos);
        auto melonds_prep = req.backends.melonds_backend->prepare(
            req.launch_config,
            MelonDsBackendPrepContext{
                req.save_profile,
                req.launch_plan.players,
                req.config.verbose,
                req.devices.product_id_base,
                req.config.ignore_controller.value_or(""),
                req.launch_env.capture.virtualgl_capture,
                req.gamescope_capture,
                req.slot,
                profile_name,
                nds_layout,
                std::move(req.devices.resolved_pads),
            });
        req.devices.resolved_pads = std::move(melonds_prep.resolved_pads);
        req.backends.melonds_backend->assign_launch_env_profile(req.launch_env.request, melonds_prep);
        log_melonds_backend_prep(
            *req.backends.melonds_backend,
            req.launch_env.request,
            melonds_prep,
            req.slot);
    } else if (req.launch_config.standalone) {
        throw std::runtime_error("standalone launch missing backend for system=" + req.system_key);
    } else {
        RetroArchOverrideParams override_params;
        override_params.first_virtual_joypad_index = req.devices.virtual_joypad_index;
        override_params.identities = &req.launch_plan.virtual_identities;
        override_params.joypad_driver = req.config.retroarch_joypad_driver;
        override_params.players = req.launch_plan.players;
        override_params.save_profile = &req.save_profile;
        override_params.realtime_pacing = req.config.audio || req.config.video;
        override_params.capture_fullscreen =
            req.launch_env.capture.capture_fullscreen && req.use_virtual_capture;
        override_params.capture_resolution = req.config.video_resolution;
        override_params.vulkan_gpu_index =
            (!req.use_virtual_capture && req.launch_env.resolved_gpu.has_value())
                ? req.launch_env.resolved_gpu->vulkan_index
                : -1;
        override_params.system_key = req.system_key;
        override_params.core_path = req.launch_config.core_path;
        override_params.resolution_scale = req.config.resolution.retroarch_scale;
        override_params.slot_index = req.slot;
        override_params.network_cmd_port = req.plan.retroarch_netcmd_port;
        {
            std::vector<ClientHello> hellos;
            hellos.reserve(req.plan.clients.size());
            for (const auto& client : req.plan.clients) {
                hellos.push_back(client.hello);
            }
            override_params.display_layout =
                resolve_display_layout_preference(req.plan.host_hello, hellos);
        }
        apply_retroarch_override(req.launch_config, override_params);
        req.launch_env.request.pad_plan = req.devices.shared_pad_plan;
        log_pad_plan(req.devices.shared_pad_plan, req.slot);
    }
}

SlotContent build_slot_content(
    GameCatalog& catalog,
    const HostLaunchPlan& launch_plan,
    std::string& system_key) {
    SlotContent content{};
    if (const auto hosted = catalog.find_hosted(launch_plan.game_id); hosted.has_value()) {
        auto hosted_ = hosted->get();
        system_key = hosted_.info.system_key;
        content.active_display_name = hosted_.info.display_name;
        content.catalog_content_path = hosted_.content_path;
        content.m3m_title_id = hosted_.m3m_title_id;
    } else if (const auto info = catalog.find(launch_plan.game_id); info.has_value()) {
        system_key = info->system_key;
        content.active_display_name = info->display_name;
    }
    return content;
}

void append_detected_host_controller_ignore_list(HostAppConfig& config, int slot) {
    try {
        ControllerBackend host_pads;
        std::string host_ignore;
        for (const auto& device : host_pads.list_devices()) {
            if (device.vendor_id == 0 || device.product_id == 0) {
                continue;
            }
            const auto id = hex_vid_pid(device.vendor_id, device.product_id);
            if (!host_ignore.empty()) {
                host_ignore += ",";
            }
            host_ignore += id;
        }
        if (!host_ignore.empty()) {
            if (config.ignore_controller.has_value() && !config.ignore_controller->empty()) {
                config.ignore_controller = *config.ignore_controller + "," + host_ignore;
            } else {
                config.ignore_controller = host_ignore;
            }
        }
    } catch (const std::exception& error) {
        std::cerr
            << "session slot " << slot << ": warning: host controller scan failed: "
            << error.what() << '\n';
    }
}

void print_session_info(
    int slot,
    const HostLaunchPlan& launch_plan,
    const RetroArchLaunchConfig& launch_config,
    const HostAppConfig& config,
    const SaveProfile& save_profile) {
    std::cout
        << "session slot " << slot << ": selected game " << launch_plan.game_id
        << "\nRetroArch: " << launch_config.retroarch_path
        << "\nCore:      " << launch_config.core_path
        << "\nContent:   " << launch_config.content_path
        << "\nMode:      " << session_mode_name(launch_plan.session_mode)
        << "\nPlayers:   " << static_cast<int>(launch_plan.players)
        << "\nHostRole:  " << participant_role_name(config.host_role)
        << "\nJoypad:    " << config.retroarch_joypad_driver
        << "\nUser:      " << save_profile.username
        << '\n';
}

void ActiveSessionSlot::cleanup(
    int slot,
    SessionBackendState& backends,
    const std::string& admin_stop_reason,
    std::optional<std::string> session_end_reason,
    const std::function<bool()>& should_stop) {
    if (!should_stop() && !session_end_reason.has_value() && !session_runtime_->emulator_running()) {
        const auto code = session_runtime_->last_exit_code().value_or(-1);
        const auto stderr_tail = session_runtime_->last_stderr_tail();
        session_end_reason = format_emulator_exit_summary(code);
        std::cerr
            << "session slot " << config_.slot_index << ": " << *session_end_reason << '\n';
        if (!stderr_tail.empty()) {
            std::cerr
                << "session slot " << config_.slot_index
                << ": emulator/gamescope stdio tail:\n"
                << stderr_tail << '\n';
        } else {
            std::cerr
                << "session slot " << config_.slot_index
                << ": (no emulator/gamescope stdio captured)\n";
        }
    }

    {
        const std::string preview = should_stop()
            ? "host stopped"
            : session_end_reason.value_or("session ended");
        std::cout
            << "session slot " << config_.slot_index << ": ending: " << preview << '\n';
    }
    // Drop XTest *before* killing gamescope/Xvfb. A live Display* to nested :N when
    // that socket disappears makes Xlib call exit(1) ("XIO: fatal IO error … :1")
    // and takes down the whole host_runner lobby with the session.
    if (input_router_ != nullptr) {
        unregister_input_clients();
        input_router_->set_touch_handler({});
        input_router_.reset();
    }
    if (melonds_touch_ctrl_ != nullptr) {
        melonds_touch_ctrl_->close_touch_channel();
        melonds_touch_ctrl_.reset();
    }
    last_ds_screen_layout_.reset();
    last_ds_screen_query_ = {};
    unplug_session_keyboard(keyboard_.get());
    keyboard_.reset();

    // The runtime destructor also stops any process it still owns; stop here so
    // teardown ordering stays predictable.
    stop_session_runtime(session_runtime_, /*reset=*/true);

    // Pull Ryujinx/Yuzu Switch saves into the shared canonical tree after exit.
    // (Launch already synced; in-session Ryujinx writes stay in bis until now.)
    if (backends.switch_backend) {
        sync_and_log_post_exit_switch_saves(
            save_profile_,
            slot,
            backends.switch_backend.get(),
            backends.switch_launch_content_stem,
            backends.switch_launch_title_id);
    }
    if (backends.melonds_backend) {
        (void)backends.melonds_backend->post_exit_sync(save_profile_);
    }

    const std::string end_reason = !admin_stop_reason.empty()
        ? admin_stop_reason
        : (should_stop()
            ? "host stopped"
            : session_end_reason.value_or("session ended"));
    shutdown_media_and_clients(end_reason);

    // Drop the null sink after capture stops so the mixer only shows live slots.
    audio_channel_.reset();
}

void ActiveSessionSlot::run_session() {
    const int slot = config_.slot_index;

    if (config_.catalog == nullptr) {
        throw std::runtime_error("session slot missing game catalog");
    }
    auto& catalog = *config_.catalog;
    auto& config = slot_config_;
    auto& launch_plan = config_.launch_plan;
    auto& plan = config_.plan;
    SessionBackendState backends;

    // Concurrent slots each own a SessionAudioChannel (archstreamer-N + app id).
    if (config.audio && should_use_slot_streaming_sink(config.audio_source)) {
        try {
            audio_channel_ = std::make_unique<SessionAudioChannel>(slot);
            config.audio_source = audio_channel_->monitor_source();
            std::cout
                << "session slot " << slot << ": audio capture "
                << config.audio_source
                << " (id " << audio_channel_->application_id() << ")\n";
        } catch (const std::exception& error) {
            audio_channel_.reset();
            std::cerr
                << "session slot " << slot << ": warning: per-slot audio sink failed: "
                << error.what() << '\n';
        }
    }

    plan.retroarch_netcmd_port =
        static_cast<std::uint16_t>(DefaultRetroArchNetcmdPort + slot);

    if (launch_plan.save_username.empty()) {
        launch_plan.save_username = default_cli_username();
    }
    if (!valid_username(launch_plan.save_username)) {
        throw std::runtime_error(
            "save username must be 1-64 characters and contain only letters, numbers, underscores, or hyphens");
    }
    if (plan.save_username.empty()) {
        plan.save_username = launch_plan.save_username;
    }

    {
        const std::uint16_t product_id_base = static_cast<std::uint16_t>(0xa517 + slot * 8);
        const std::string pulse_sink = audio_channel_
            ? StreamingAudioSink::slot_sink_name(slot)
            : std::string{};

        const std::string pulse_app = audio_channel_
            ? StreamingAudioSink::slot_application_id(slot)
            : std::string{};
        cadence_tracker_.begin(
            slot,
            plan.save_username,
            plan.selected_game_id,
            plan.system_key,
            session_mode_name(plan.session_mode),
            config.virtual_display,
            config.video_port,
            config.audio_port,
            plan.retroarch_netcmd_port,
            pulse_sink,
            pulse_app,
            product_id_base);
    }

    if (launch_plan.virtual_identities.size() < launch_plan.players) {
        launch_plan.virtual_identities.resize(launch_plan.players);
    }

    save_profile_ = prepare_save_profile(config.save_root, launch_plan.save_username);

    auto launch_config = catalog.launch_config_for(launch_plan.game_id);
    const auto resolved_retroarch = resolve_retroarch();
    system_key_.clear();
    SlotContent content = build_slot_content(catalog, launch_plan, system_key_);
    plan.system_key = system_key_;

    // Advertise this live save profile to the host Users browser (cleared on exit).
    ActiveSaveSessionGuard active_save_guard{config.save_root, slot};
    {
        ActiveSaveSession active;
        active.username = launch_plan.save_username;
        active.game_id = launch_plan.game_id;
        active.system_key = system_key_;
        active.display_name = content.active_display_name;
        // Keep catalog path (.m3m when present) so save stem stays on the map entry.
        active.content_path = content.catalog_content_path.empty()
            ? launch_config.content_path.string()
            : content.catalog_content_path.string();
        active.slot_index = slot;
        publish_active_save_session(config.save_root, active);
        record_user_game_played(active.username, active.game_id, active.system_key);
    }

    std::string admin_stop_reason;
    auto should_stop = [this, &config, slot, &admin_stop_reason]() {
        if (stop_requested_.load()) {
            return true;
        }
        if (auto reason = take_active_session_stop_request(config.save_root, slot);
            reason.has_value()) {
            admin_stop_reason = *reason;
            stop_requested_.store(true);
            std::cerr
                << "session slot " << slot << ": stop requested (" << admin_stop_reason << ")\n";
            return true;
        }
        if (config_.should_stop && config_.should_stop()) {
            return true;
        }
        return false;
    };


    // Advertise clients as Connected while the emulator boots (before Active).
    for (const auto& client : plan.clients) {
        if (client.hello.username.empty()) {
            continue;
        }
        ConnectedClientPresence presence;
        presence.username = client.hello.username;
        presence.client_id = client.client_id;
        presence.slot_index = slot;
        presence.game_id = plan.selected_game_id;
        presence.phase = "session";
        presence.seated = client.hello.requested_players > 0;
        publish_connected_client(config.save_root, presence);
    }

    if (!launch_config.standalone && system_key_ == "ps2") {
        std::cout
            << "session slot " << slot << ": PS2 memcards "
            << user_ps2_memcard_directory(save_profile_) << '\n';
    }

#if !defined(_WIN32)
    if (!launch_config.standalone &&
        (system_key_ == "ps1" || system_key_ == "ps2" || system_key_ == "psp") &&
        config.retroarch_joypad_driver == "sdl2") {
        std::cout
            << "session slot " << slot << ": forcing joypad driver udev for " << system_key_
            << " (sdl2 stalls PlayStation cores).\n";
        config.retroarch_joypad_driver = "udev";
    }
#endif

#if defined(ARCHSTREAMER_DEBUG_GB_LINK)
    if (system_key_ == "gb" || system_key_ == "gbc" || system_key_ == "gb-gbc") {
        LinkCableBackend::write_single_gb_core_options();
    }
#endif

    if (const auto hosted = catalog.find_hosted(launch_plan.game_id); hosted.has_value()) {
        if (plan.playlist_discs.empty()) {
            plan.playlist_discs = hosted->get().info.playlist_discs;
            plan.current_disc_index = 0;
        }
        if (!plan.playlist_discs.empty()) {
            std::cout
                << "session slot " << slot << ": multi-disc playlist "
                << plan.playlist_discs.size() << " disc(s); netcmd port "
                << plan.retroarch_netcmd_port << '\n';
        }
    }

    if (!launch_config.standalone) {
        launch_config.retroarch_path = resolved_retroarch.display_path;
        launch_config.command_prefix = resolved_retroarch.argv_prefix;
    }
    if (config.verbose && !launch_config.standalone) {
        launch_config.extra_args.insert(launch_config.extra_args.begin(), "--verbose");
    }

    const bool host_plays_locally =
        config.host_role == ParticipantRole::Player && config_.bridge_device.has_value();

    if (config_.bridge_device.has_value() && !config.ignore_controller.has_value()) {
        if (config_.bridge_device->vendor_id != 0 && config_.bridge_device->product_id != 0) {
            config.ignore_controller =
                hex_vid_pid(config_.bridge_device->vendor_id, config_.bridge_device->product_id);
        }
    }

    append_detected_host_controller_ignore_list(config, slot);

    auto ignore_devices = config.ignore_controller.value_or("");
    const char* steam_input = "0x28de/0x11ff,0x28de/0x1205,0x28de/0x1201";
    if (ignore_devices.empty()) {
        ignore_devices = steam_input;
    } else {
        ignore_devices = ignore_devices + "," + steam_input;
    }
    config.ignore_controller = ignore_devices;

    auto launch_env = prepare_slot_launch_environment(
        config,
        launch_config,
        slot,
        host_plays_locally,
        config_.session_id);
    use_virtual_capture_ = launch_env.capture.use_virtual_capture;
    gamescope_capture_ = launch_env.capture.gamescope_capture;

    auto media = prepare_slot_media_plan(config, plan);

    print_session_info(slot, launch_plan, launch_config, config, save_profile_);

    if (config.dry_run) {
        for (const auto& stream : media.streams) {
            if (stream.client_id == HostClientId) {
                continue;
            }
            send_media_endpoint_to_client(plan, stream.client_id, stream.endpoint);
        }
        send_session_starting_to_clients(plan);
        send_session_ended_to_clients(plan, "dry run complete");
        return;
    }

    apply_slot_product_id_offset(launch_plan.virtual_identities, slot);
    gamepads_ = std::make_unique<HostVirtualGamepadBus>(launch_plan.virtual_identities);
    plug_session_gamepads(*gamepads_, launch_plan.players);

    keyboard_ = std::make_unique<VirtualKeyboard>(launch_env.xtest_display);
    keyboard_->set_netcmd_port(plan.retroarch_netcmd_port);
    wait_for_session_input_enumeration();

    auto devices = resolve_slot_device_plan(config, launch_plan, slot);
    apply_capture_to_session_device_plan(
        devices,
        config,
        launch_env.capture,
        launch_env.resolved_gpu);
    virtual_joypad_index_ = devices.virtual_joypad_index;

    prepare_slot_backend(SlotBackendPrepareRequest{
        slot,
        config,
        plan,
        launch_plan,
        launch_config,
        save_profile_,
        content,
        system_key_,
        use_virtual_capture_,
        gamescope_capture_,
        keyboard_.get(),
        launch_env,
        devices,
        backends,
    });

    apply_capture_and_launch_environment(
        launch_config,
        launch_env.capture,
        config,
        launch_env.gamescope_vk_device,
        launch_env.resolved_gpu,
        launch_env.request);

    // Channel owns the authoritative Pulse identity for this slot.
    if (audio_channel_ != nullptr) {
        ProcessEnvironment env;
        env.merge_pairs(launch_config.environment);
        for (const auto& key : launch_config.unset_environment) {
            env.add_unset(key);
        }
        env.merge(audio_channel_->launch_env());
        launch_config.environment = std::move(env.entries);
        launch_config.unset_environment = std::move(env.unset);
    }

    if (config.audio) {
        park_session_game_audio(audio_channel_.get());
    }

    input_router_ = std::make_unique<InputRouter>(*gamepads_, keyboard_.get());
    melonds_touch_ctrl_ = configure_slot_input_router(
        *input_router_,
        *keyboard_,
        launch_plan,
        backends);
    // Register demux before emulator start so early UDP from a phone client is not
    // dropped during the multi-second launch window.
    register_input_clients();

    media_server_ = start_host_media_server_if_needed(HostMediaStartRequest{
        config,
        launch_env.capture.capture_display,
        launch_env.capture.display_backend,
        launch_env.nvenc_cuda_device_id,
        media.config,
        media.destinations,
        media.streams,
    });
    if (media_server_ != nullptr) {
        media_index_ = media.destinations.size();
    }

    const auto slot_prefix = "session slot " + std::to_string(slot) + ": ";
    plug_virtual_keyboard_with_retry(
        *keyboard_,
        use_virtual_capture_,
        gamescope_capture_,
        slot_prefix);

    for (const auto& stream : media.streams) {
        if (stream.client_id == HostClientId) {
            continue;
        }
        if (!stream.endpoint.video_uri.empty() || !stream.endpoint.audio_uri.empty()) {
            send_media_endpoint_to_client(plan, stream.client_id, stream.endpoint);
        }
    }

    send_session_starting_to_clients(plan);

    if (media_server_ != nullptr) {
        session_monitor_.emplace(
            plan,
            *input_router_,
            *media_server_,
            std::chrono::seconds(config.client_timeout_seconds),
            std::chrono::seconds(config.player_reconnect_timeout_seconds),
            config_.hub,
            media.capture_w,
            media.capture_h,
            config.save_root,
            slot,
            cadence_tracker_.session_id());
    }

    auto local_bridge = std::optional<LocalControllerBridge>{};
    if (config_.bridge_device.has_value()) {
        local_bridge.emplace(*config_.bridge_device);
    }

    session_runtime_ = make_session_runtime(launch_plan);
    session_runtime_->bind_launch_config(std::move(launch_config));
    std::cout
        << "session slot " << slot << ": launching "
        << session_runtime_->kind_name() << '\n';

    start_emulator_and_verify(*session_runtime_, EmulatorStartFailDetail::Brief);
    post_emulator_start_warmup(
        media_server_.get(),
        config,
        media.streams,
        *session_runtime_,
        /*audio=*/nullptr,
        /*audio_slot_index=*/std::nullopt,
        *gamepads_,
        launch_plan.players,
        keyboard_.get(),
        gamescope_capture_,
        launch_env.xtest_display,
        slot_prefix,
        audio_channel_.get());

    {
        print_session_data(slot, plan);
        cadence_session_live_ = true;

        if (const auto pid = session_runtime_->emulator().process_id(); pid.has_value()) {
            cadence_tracker_.claim_emulator_pid(*pid);
        }
        for (const auto& client : plan.clients) {
            if (client.hello.username.empty()) {
                continue;
            }
            record_client_joined(
                slot,
                client.hello.username,
                plan.selected_game_id,
                client.hello.requested_players > 0 ? "player" : "viewer",
                cadence_tracker_.session_id());
            ConnectedClientPresence presence;
            presence.username = client.hello.username;
            presence.client_id = client.client_id;
            presence.slot_index = slot;
            presence.game_id = plan.selected_game_id;
            presence.phase = "session";
            presence.seated = client.hello.requested_players > 0;
            publish_connected_client(config.save_root, presence);
        }
        if (plan.host_hello.has_value() && !plan.host_hello->username.empty()) {
            record_client_joined(
                slot,
                plan.host_hello->username,
                plan.selected_game_id,
                "host",
                cadence_tracker_.session_id());
        }
    }

    if (devices.arm_soft_keyboard && plan.soft_keyboard) {
        std::string display = launch_env.xtest_display;
        if (keyboard_ != nullptr && keyboard_->plugged()) {
            display = keyboard_->capture_display();
        }
        schedule_soft_keyboard(
            plan.soft_keyboard,
            devices.soft_keyboard_fallback,
            // Prefer OCR of Ryujinx HeaderText when the dialog appears.
            {},
            display,
            session_runtime_->emulator().process_id().value_or(0));
    }

    std::optional<std::string> session_end_reason;
    const RelaunchContext relaunch_ctx{
        launch_env.capture.capture_fullscreen,
        &launch_env.resolved_gpu,
        &launch_env.request,
    };
    SessionLoopCadence loop_cadence(
        local_bridge.has_value() ? &*local_bridge : nullptr,
        input_router_.get(),
        /*audio=*/nullptr,
        /*audio_slot_index=*/std::nullopt,
        config.audio,
        audio_channel_.get());

    poll_while(should_stop, session_end_reason, relaunch_ctx);

    cleanup(slot, backends, admin_stop_reason, session_end_reason, should_stop);
}

void ActiveSessionSlot::print_session_data(int slot, const SessionPlan& plan) {
    std::ostringstream detail;
    detail << session_mode_name(plan.session_mode);
    if (!plan.system_key.empty()) {
        detail << " system=" << plan.system_key;
    }
    record_session_started(
        slot,
        plan.save_username,
        plan.selected_game_id,
        detail.str(),
        cadence_tracker_.session_id());
}


void ActiveSessionSlot::poll_while(
    const std::function<bool()>& should_stop,
    std::optional<std::string>& session_end_reason,
    const RelaunchContext& relaunch_ctx) {
    while (!should_stop() && session_runtime_->emulator_running()) {
        drain_pending_joins();
        if (const auto reason = poll_session_monitor_stop(); reason.has_value()) {
            session_end_reason = reason;
            break;
        }
        if (const auto reason = handle_pending_link_promotion(); reason.has_value()) {
            session_end_reason = reason;
            break;
        }
        if (const auto reason = handle_gb_link_relaunch(relaunch_ctx); reason.has_value()) {
            session_end_reason = reason;
            break;
        }
        if (const auto reason = handle_gba_netplay_relaunch(relaunch_ctx); reason.has_value()) {
            session_end_reason = reason;
            break;
        }

        // Ryujinx may rewrite Config.json to WindowKeyboard if the pad was missing
        // or grabbed at first poll; re-assert GamepadSDL2 a few times early on.
        if (ryujinx.ryujinx_reassert_remaining > 0 && ryujinx.ryujinx_control_root.has_value()) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= ryujinx.ryujinx_next_reassert) {
                (void)RyujinxControls::reassert_archstreamer_controls_if_needed(
                    *ryujinx.ryujinx_control_root,
                    ryujinx.ryujinx_control_pads,
                    ryujinx.ryujinx_control_filter);
                --ryujinx.ryujinx_reassert_remaining;
                ryujinx.ryujinx_next_reassert = now + std::chrono::seconds(5);
            }
        }

        // Poll melonDS screen AABBs so client touch hit-target follows swap/emphasis.
        if (melonds_touch_ctrl_ != nullptr) {
            const auto now = std::chrono::steady_clock::now();
            constexpr auto kDsScreenPollInterval = std::chrono::milliseconds(250);
            if (last_ds_screen_query_.time_since_epoch().count() == 0 ||
                now - last_ds_screen_query_ >= kDsScreenPollInterval) {
                last_ds_screen_query_ = now;
                DsScreenLayout layout;
                if (melonds_touch_ctrl_->query_screens(layout)) {
                    if (!last_ds_screen_layout_.has_value() ||
                        *last_ds_screen_layout_ != layout) {
                        last_ds_screen_layout_ = layout;
                        broadcast_ds_screen_layout(config_.plan, layout);
                    }
                }
            }
        }
    }
}

} // namespace archstreamer
