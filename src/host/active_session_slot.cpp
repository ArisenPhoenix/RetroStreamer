#include "host/active_session_slot.hpp"

#include "common/cli_common.hpp"
#include "common/participant_role.hpp"
#include "common/serialization.hpp"
#include "host/cadence_session_events.hpp"
#include "host/cadence_session_tracker.hpp"
#include "host/capture_platform.hpp"
#include "host/client_stream_policy.hpp"
#include "host/game_catalog.hpp"
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
#include "host/session_launch_types.hpp"
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
    SessionLaunchEnvironment session;
    std::string xtest_display;
};

SlotLaunchEnvironment prepare_slot_launch_environment(
    HostAppConfig& config,
    RetroArchLaunchConfig& launch_config,
    int slot,
    bool host_plays_locally,
    const SessionId& session_id) {
    SlotLaunchEnvironment env;
    env.session.capture = resolve_capture_plan(config, launch_config);
    env.session.request.stream_media = config.audio || config.video;
    env.session.request.stream_audio = config.audio;
    env.session.request.host_plays_locally = host_plays_locally;
    env.session.request.audio_source = config.audio_source;
    env.session.request.ignore_devices = *config.ignore_controller;
    env.session.request.use_virtual_capture = env.session.capture.use_virtual_capture;
    env.session.request.gamescope_capture = env.session.capture.gamescope_capture;
    env.session.request.virtualgl_capture = env.session.capture.virtualgl_capture;
    env.session.request.capture_display = env.session.capture.capture_display;
    env.xtest_display = env.session.capture.gamescope_capture
        ? gamescope_xtest_display_for_slot(slot)
        : env.session.capture.capture_display;
    if (env.session.capture.gamescope_capture) {
        env.session.request.xtest_display = env.xtest_display;
    }
    if (!session_id.empty()) {
        env.session.request.session_id = session_id;
        if (env.session.capture.gamescope_capture || env.session.capture.use_virtual_capture) {
            register_session_xtest_display(session_id, env.xtest_display);
            std::cout
                << "session slot " << slot << ": session " << session_id
                << " XTest lease " << env.xtest_display << '\n';
        }
    }

    const auto gpu = resolve_session_gpu_selection(
        config,
        env.session.capture,
        "session slot " + std::to_string(slot) + ": ");
    env.session.gpu = std::move(gpu);
    apply_session_gpu_to_launch_request(env.session.request, env.session.gpu);
    return env;
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
    override_params.capture_fullscreen = ctx.capture.capture_fullscreen && use_virtual_capture_;
    override_params.capture_resolution = ctx.capture.video_resolution;
    const bool have_gpu = ctx.capture.resolved_gpu.has_value();
    override_params.vulkan_gpu_index =
        (!use_virtual_capture_ && have_gpu) ? ctx.capture.resolved_gpu->vulkan_index : -1;
    override_params.system_key = system_key_;
    override_params.core_path = core_path;
    override_params.resolution_scale = slot_config_.resolution.retroarch_scale;
    override_params.slot_index = config_.slot_index;
    override_params.network_cmd_port = config_.plan.retroarch_netcmd_port;
    override_params.display_layout =
        resolve_session_plan_participants(save_profile_.username, config_.plan).display_layout;
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
    if (!plan.link.pending_promotion) {
        return std::nullopt;
    }
    plan.link.pending_promotion = false;
    LinkPromotionRequest promotion;
    promotion.logical_host_client_id = plan.link.pending_host_client_id;
    promotion.logical_client_client_id = plan.link.pending_client_client_id;
    promotion.logical_host_username = plan.link.pending_host_username;
    promotion.logical_client_username = plan.link.pending_client_username;
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
        !plan.link.cable.consume_relaunch_request()) {
        return std::nullopt;
    }
#if defined(ARCHSTREAMER_DEBUG_GB_LINK)
    const auto link_core = plan.link.cable.pending_core_path();
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

void ActiveSessionSlot::publish_connected_presence(
    ClientId client_id,
    const ClientHello& hello,
    const SessionPlan& plan) const {
    ConnectedClientPresence presence;
    presence.username = hello.username;
    presence.client_id = client_id;
    presence.slot_index = config_.slot_index;
    presence.game_id = plan.selected_game_id;
    presence.phase = "session";
    presence.seated = hello.requested_players > 0;
    publish_connected_client(config_.host_config.save_root, presence);
}

SessionClientConnection* ActiveSessionSlot::attach_pending_join(
    ClientId client_id,
    const MediaEndpoint& endpoint,
    SessionClientConnection* reconnecting_client,
    PendingJoin& pending,
    SessionPlan& plan) {
    if (reconnecting_client != nullptr) {
        reset_reconnected_session_client(
            *reconnecting_client,
            pending.hello,
            std::move(pending.stream),
            plan,
            endpoint);
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
        publish_connected_presence(client_id, pending.hello, plan);
        return reconnecting_client;
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
        publish_connected_presence(client_id, pending.hello, plan);
        return &plan.clients.back();
    }
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
            if (media_server_ == nullptr) {
                throw std::runtime_error("media server unavailable for pending join");
            }

            // Same control handshake as a fresh lobby join / poll_active_session_joins:
            // Welcome → Seats → Ready → MediaEndpoint → SessionStarting.
            // Skipping Welcome left Android (and any ClientHello waiter) hanging on an
            // open TCP with no heartbeats until the reconnect seat timed out.
            const auto join = resolve_live_session_join_target(
                plan,
                pending.hello,
                pending.is_reconnect);
            auto endpoint = send_live_session_join_handshake(LiveSessionJoinHandshake{
                pending.stream,
                pending.hello,
                join.client_id,
                plan,
                media_plan_config_for(slot_config_),
                media_index_,
                *media_server_,
            });
            auto* joined_client =
                attach_pending_join(
                    join.client_id,
                    endpoint,
                    join.reconnecting_client,
                    pending,
                    plan);

            if (config_.input_demux != nullptr && input_router_ != nullptr) {
                config_.input_demux->register_router(join.client_id, input_router_.get());
            }

            // Late join / reconnect: push current bottom-screen hit target if known.
            if (last_ds_screen_layout_.has_value()) {
                send_ds_screen_layout_to_client(*joined_client, *last_ds_screen_layout_);
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

struct SlotBackendPrepareRequest {
    int slot = 0;
    SessionPlan& plan;
    SessionBackendPrepareContext& backend;
    bool use_virtual_capture = false;
    bool gamescope_capture = false;
    VirtualKeyboard* keyboard = nullptr;
};

void prepare_slot_backend(const SlotBackendPrepareRequest& req) {
    const auto& config = req.backend.config;
    auto& user = req.backend.user;
    auto& game = req.backend.game;
    auto& video = req.backend.video;
    auto& input = req.backend.input;
    auto& launch_plan = game.launch_plan;
    auto& launch_config = game.launch_config;
    auto& save_profile = user.save_profile;
    const auto& system_key = game.content.system_key;
    auto& devices = input.devices;
    auto& backends = req.backend.backends;
    auto& launch_env_request = req.backend.launch_env_request;

    try {
        prepare_session_standalone_backend(system_key, launch_config, backends);
    } catch (const std::runtime_error& error) {
        send_error_to_session_clients(req.plan, error.what());
        throw;
    }

    if (backends.switch_backend && req.keyboard != nullptr) {
        req.keyboard->set_switch_style_hotkeys(true);
    }

    if (backends.switch_backend) {
        const auto prefer_handheld_mode = session_prefers_switch_handheld_mode(req.plan);
        const auto switch_content =
            resolve_switch_launch_content(save_profile, launch_config, game.content);
        auto switch_prep = backends.switch_backend->prepare(
            launch_config,
            SwitchBackendPrepContext{
                save_profile,
                launch_plan.players,
                config.verbose,
                devices.input.product_id_base,
                config.ignore_controller.value_or(""),
                config.graphics_api,
                video.capture.virtualgl_capture,
                req.gamescope_capture,
                video.switch_scale,
                prefer_handheld_mode,
                &video.devices.resolved_gpu,
                user.participants.profile_display_name,
                std::move(devices.input.resolved_pads),
                static_cast<std::size_t>(std::max(0, req.slot)),
                launch_plan.game_id,
                switch_content.content_stem,
                switch_content.title_id,
            });
        devices.input.resolved_pads = std::move(switch_prep.resolved_pads);
        backends.switch_backend->assign_launch_env_profile(launch_env_request, switch_prep);
        log_switch_backend_prep(
            *backends.switch_backend,
            launch_env_request,
            switch_prep,
            video.switch_scale,
            video.devices.resolved_gpu,
            req.slot);
        backends.switch_launch_content_stem = switch_content.content_stem;
        backends.switch_launch_title_id = switch_content.title_id;
        if (backends.switch_backend->enable_soft_keyboard()) {
            if (!req.plan.soft_keyboard) {
                req.plan.soft_keyboard = std::make_shared<SoftKeyboardHostBridge>();
            }
            devices.keyboard.soft_keyboard_fallback = user.participants.profile_display_name;
            devices.keyboard.arm_soft_keyboard = true;
        }
    } else if (backends.melonds_backend) {
        auto melonds_prep = backends.melonds_backend->prepare(
            launch_config,
            MelonDsBackendPrepContext{
                save_profile,
                launch_plan.players,
                config.verbose,
                devices.input.product_id_base,
                config.ignore_controller.value_or(""),
                video.capture.virtualgl_capture,
                req.gamescope_capture,
                req.slot,
                user.participants.profile_display_name,
                user.participants.display_layout,
                std::move(devices.input.resolved_pads),
            });
        devices.input.resolved_pads = std::move(melonds_prep.resolved_pads);
        backends.melonds_backend->assign_launch_env_profile(launch_env_request, melonds_prep);
        log_melonds_backend_prep(
            *backends.melonds_backend,
            launch_env_request,
            melonds_prep,
            req.slot);
    } else if (launch_config.standalone) {
        throw std::runtime_error("standalone launch missing backend for system=" + system_key);
    } else {
        const auto override_params = build_session_retroarch_override(
            req.backend,
            SessionRetroArchOverrideOptions{
                req.use_virtual_capture,
                req.slot,
                req.plan.retroarch_netcmd_port,
                user.participants.display_layout,
            });
        apply_retroarch_override(launch_config, override_params);
        launch_env_request.pad_plan = devices.input.shared_pad_plan;
        log_pad_plan(devices.input.shared_pad_plan, req.slot);
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

    auto launch_context = prepare_session_launch_context(
        config,
        catalog,
        launch_plan,
        "session slot " + std::to_string(slot) + ": ");
    auto& launch_assets = launch_context.assets;
    save_profile_ = launch_assets.save_profile;
    auto& launch_config = launch_assets.launch_config;
    const auto& content = launch_assets.content;
    system_key_.clear();
    system_key_ = content.system_key;
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
        publish_connected_presence(client.client_id, client.hello, plan);
    }

#if defined(ARCHSTREAMER_DEBUG_GB_LINK)
    if (system_key_ == "gb" || system_key_ == "gbc" || system_key_ == "gb-gbc") {
        LinkCableBackend::write_single_gb_core_options();
    }
#endif

    if (plan.playlist_discs.empty()) {
        plan.playlist_discs = content.playlist_discs;
        plan.current_disc_index = 0;
    }
    if (!plan.playlist_discs.empty()) {
        std::cout
            << "session slot " << slot << ": multi-disc playlist "
            << plan.playlist_discs.size() << " disc(s); netcmd port "
            << plan.retroarch_netcmd_port << '\n';
    }

    const bool host_plays_locally =
        config.host_role == ParticipantRole::Player && config_.bridge_device.has_value();

    append_session_controller_ignore_list(
        config,
        config_.bridge_device,
        "session slot " + std::to_string(slot) + ": ");

    auto launch_env = prepare_slot_launch_environment(
        config,
        launch_config,
        slot,
        host_plays_locally,
        config_.session_id);
    use_virtual_capture_ = launch_env.session.capture.use_virtual_capture;
    gamescope_capture_ = launch_env.session.capture.gamescope_capture;

    auto media = build_session_media_plan(
        config,
        &plan,
        SessionMediaPlanKind::Slot);

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

    // Concurrent slots must not see each other's ArchStreamer uinput pads. Exclusive
    // PadPlan IGNORES sibling VID/PIDs (EXCEPT alone fails for uinput under Ryujinx).
    auto devices = resolve_session_device_plan(
        config,
        launch_plan,
        SessionPadPlanKind::RetroArchSlot,
        static_cast<std::uint16_t>(0xa517 + slot * 8));
    apply_capture_to_session_device_plan(
        devices,
        config,
        launch_env.session.capture,
        launch_env.session.gpu.resolved_gpu);
    virtual_joypad_index_ = devices.input.virtual_joypad_index;

    auto backend_context = make_session_backend_prepare_context(
        config,
        launch_plan,
        launch_assets,
        devices,
        backends,
        launch_env.session.capture,
        launch_env.session.request,
        resolve_session_plan_participants(save_profile_.username, plan));
    prepare_slot_backend(SlotBackendPrepareRequest{
        slot,
        plan,
        backend_context,
        use_virtual_capture_,
        gamescope_capture_,
        keyboard_.get(),
    });

    apply_capture_and_launch_environment(
        launch_config,
        launch_env.session.capture,
        config,
        launch_env.session.gpu.gamescope_vk_device,
        launch_env.session.gpu.resolved_gpu,
        launch_env.session.request);

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
    melonds_touch_ctrl_ = configure_session_input_router(
        *input_router_,
        *keyboard_,
        launch_plan,
        backends);
    // Register demux before emulator start so early UDP from a phone client is not
    // dropped during the multi-second launch window.
    register_input_clients();

    media_server_ = start_host_media_server_if_needed(HostMediaStartRequest{
        config,
        SessionMediaCaptureContext{
            launch_env.session.capture.capture_display,
            launch_env.session.capture.display_backend,
            launch_env.session.gpu.nvenc_cuda_device_id,
        },
        SessionMediaStreamContext{
            media.config,
            media.destinations,
            media.streams,
        },
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
            publish_connected_presence(client.client_id, client.hello, plan);
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

    if (devices.keyboard.arm_soft_keyboard && plan.soft_keyboard) {
        std::string display = launch_env.xtest_display;
        if (keyboard_ != nullptr && keyboard_->plugged()) {
            display = keyboard_->capture_display();
        }
        schedule_soft_keyboard(
            plan.soft_keyboard,
            devices.keyboard.soft_keyboard_fallback,
            // Prefer OCR of Ryujinx HeaderText when the dialog appears.
            {},
            display,
            session_runtime_->emulator().process_id().value_or(0));
    }

    std::optional<std::string> session_end_reason;
    const RelaunchContext relaunch_ctx{
        SessionCaptureDevices{
            launch_env.session.gpu.resolved_gpu,
            launch_env.session.capture.use_virtual_capture,
            launch_env.session.capture.capture_fullscreen,
            launch_env.session.capture.capture_display,
            launch_env.session.capture.display_backend,
            config.video_resolution,
        },
        &launch_env.session.request,
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
