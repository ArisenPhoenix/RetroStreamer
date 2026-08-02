#include "host/active_session_slot.hpp"

#include "common/cli_common.hpp"
#include "common/participant_role.hpp"
#include "common/serialization.hpp"
#include "client/controller_backend.hpp"
#include "host/capture_platform.hpp"
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
#include "host/session_lobby.hpp"
#include "host/session_launch_assemble.hpp"
#include "host/session_run_helpers.hpp"
#include "host/standalone_emulator.hpp"
#include "host/switch/switch_backend.hpp"
#include "host/virtual_joypad_resolve.hpp"
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
        return;
    }
    for (const auto& seat : config_.plan.seats.seats) {
        config_.input_demux->register_router(seat.client_id, input_router_.get());
    }
}

void ActiveSessionSlot::unregister_input_clients() {
    if (config_.input_demux == nullptr || input_router_ == nullptr) {
        return;
    }
    config_.input_demux->unregister_all_for(input_router_.get());
}

void ActiveSessionSlot::shutdown_media_and_clients(const std::string& end_reason) {
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
                std::cout
                    << "session slot " << config_.slot_index << ": player "
                    << static_cast<int>(client_id)
                    << " reconnected username=" << pending.hello.username << ".\n";
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
            }

            if (config_.input_demux != nullptr && input_router_ != nullptr) {
                config_.input_demux->register_router(client_id, input_router_.get());
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

void ActiveSessionSlot::run_session() {
    const int slot = config_.slot_index;
    auto should_stop = [this]() {
        if (stop_requested_.load()) {
            return true;
        }
        if (config_.should_stop && config_.should_stop()) {
            return true;
        }
        return false;
    };

    if (config_.catalog == nullptr) {
        throw std::runtime_error("session slot missing game catalog");
    }
    auto& catalog = *config_.catalog;
    auto& config = slot_config_;
    auto& launch_plan = config_.launch_plan;
    auto& plan = config_.plan;
    std::unique_ptr<SwitchBackend> switch_backend;

    // Concurrent slots each get archstreamer-N so pulsesrc does not mix sessions.
    if (config.audio && config_.streaming_audio != nullptr &&
        should_use_slot_streaming_sink(config.audio_source)) {
        try {
            config.audio_source =
                config_.streaming_audio->monitor_source_for_slot(slot);
            std::cout
                << "session slot " << slot << ": audio capture "
                << config.audio_source << '\n';
        } catch (const std::exception& error) {
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
    if (launch_plan.virtual_identities.size() < launch_plan.players) {
        launch_plan.virtual_identities.resize(launch_plan.players);
    }

    save_profile_ = prepare_save_profile(config.save_root, launch_plan.save_username);

    auto launch_config = catalog.launch_config_for(launch_plan.game_id);
    const auto resolved_retroarch = resolve_retroarch();
    system_key_.clear();
    if (const auto hosted = catalog.find_hosted(launch_plan.game_id); hosted.has_value()) {
        system_key_ = hosted->get().info.system_key;
    } else if (const auto info = catalog.find(launch_plan.game_id); info.has_value()) {
        system_key_ = info->system_key;
    }
    plan.system_key = system_key_;

    if (!launch_config.standalone && system_key_ == "ps2") {
        std::cout
            << "session slot " << slot << ": PS2 memcards "
            << user_ps2_memcard_directory(save_profile_) << '\n';
    }

    if (!launch_config.standalone &&
        (system_key_ == "ps1" || system_key_ == "ps2" || system_key_ == "psp") &&
        config.retroarch_joypad_driver == "sdl2") {
        std::cout
            << "session slot " << slot << ": forcing joypad driver udev for " << system_key_
            << " (sdl2 stalls PlayStation cores).\n";
        config.retroarch_joypad_driver = "udev";
    }

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

    {
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

    auto ignore_devices = config.ignore_controller.value_or("");
    const char* steam_input = "0x28de/0x11ff,0x28de/0x1205,0x28de/0x1201";
    if (ignore_devices.empty()) {
        ignore_devices = steam_input;
    } else {
        ignore_devices = ignore_devices + "," + steam_input;
    }
    config.ignore_controller = ignore_devices;

    const auto capture = resolve_capture_plan(config, launch_config);
    use_virtual_capture_ = capture.use_virtual_capture;
    const bool capture_fullscreen = capture.capture_fullscreen;
    const std::string capture_display = capture.capture_display;
    const auto display_backend = capture.display_backend;
    gamescope_capture_ = capture.gamescope_capture;
    const bool virtualgl_capture = capture.virtualgl_capture;

    EmulatorLaunchEnvRequest launch_env_request;
    launch_env_request.stream_media = config.audio || config.video;
    launch_env_request.stream_audio = config.audio;
    launch_env_request.host_plays_locally = host_plays_locally;
    launch_env_request.audio_source = config.audio_source;
    launch_env_request.ignore_devices = *config.ignore_controller;
    launch_env_request.use_virtual_capture = use_virtual_capture_;
    launch_env_request.gamescope_capture = gamescope_capture_;
    launch_env_request.virtualgl_capture = virtualgl_capture;
    launch_env_request.capture_display = capture_display;

    auto resolved_encode = resolve_render_gpu(config.encode_gpu);
    auto resolved_gpu = resolve_render_gpu(effective_render_gpu_selection(config));
    std::string gamescope_vk_device;
    int nvenc_cuda_device_id = -1;
    if (resolved_encode.has_value() && resolved_encode->nvidia_index >= 0) {
        nvenc_cuda_device_id = resolved_encode->nvidia_index;
    }
    if (resolved_gpu.has_value()) {
        if (resolved_encode.has_value() && resolved_encode->id == resolved_gpu->id) {
            std::cout
                << "session slot " << slot << ": GPU " << resolved_gpu->name
                << " [" << resolved_gpu->id << "] (encode+render)\n";
        } else {
            if (resolved_encode.has_value()) {
                std::cout
                    << "session slot " << slot << ": encode GPU " << resolved_encode->name
                    << " [" << resolved_encode->id << "]\n";
            }
            std::cout
                << "session slot " << slot << ": render GPU " << resolved_gpu->name
                << " [" << resolved_gpu->id << "]\n";
        }
        if (const auto vd = pci_vendor_device_id(resolved_gpu->pci_bus); vd.has_value()) {
            gamescope_vk_device = *vd;
        }
        launch_env_request.render_gpu = *resolved_gpu;
    } else if (resolved_encode.has_value()) {
        std::cout
            << "session slot " << slot << ": encode GPU " << resolved_encode->name
            << " [" << resolved_encode->id << "]\n";
    }
    if (gamescope_vk_device.empty()) {
        gamescope_vk_device = "10de:2504";
    }

    const auto media_config = media_plan_config_for(config);
    auto media_destinations = std::vector<HostMediaDestination>{};
    auto media_streams = std::vector<MediaClientStream>{};
    if (config.video || config.audio) {
        media_destinations = media_destinations_for_session(media_config, plan);
        media_streams = media_streams_for_dry_run(media_config, media_destinations);
    }

    std::cout
        << "session slot " << slot << ": selected game " << launch_plan.game_id
        << "\nRetroArch: " << launch_config.retroarch_path
        << "\nCore:      " << launch_config.core_path
        << "\nContent:   " << launch_config.content_path
        << "\nMode:      " << session_mode_name(launch_plan.session_mode)
        << "\nPlayers:   " << static_cast<int>(launch_plan.players)
        << "\nHostRole:  " << participant_role_name(config.host_role)
        << "\nJoypad:    " << config.retroarch_joypad_driver
        << "\nUser:      " << save_profile_.username
        << '\n';

    if (config.dry_run) {
        for (const auto& stream : media_streams) {
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
    const std::uint16_t product_id_base =
        static_cast<std::uint16_t>(0xa517 + slot * 8);

    gamepads_ = std::make_unique<HostVirtualGamepadBus>(launch_plan.virtual_identities);
    for (RetroArchPort port = 0; port < launch_plan.players; ++port) {
        gamepads_->plug(port);
    }

    keyboard_ = std::make_unique<VirtualKeyboard>(capture_display);
    std::this_thread::sleep_for(std::chrono::milliseconds(750));

    std::vector<std::size_t> resolved_indices;
    std::vector<ArchStreamerSdlPad> resolved_pads;
    if (config.retroarch_joypad_driver == "udev") {
        resolved_indices = find_archstreamer_udev_joypad_indices(
            launch_plan.players,
            config.verbose,
            product_id_base);
    } else {
        resolved_pads = find_archstreamer_sdl_pads(
            launch_plan.players,
            config.ignore_controller.value_or(""),
            config.verbose,
            product_id_base);
        resolved_indices.reserve(resolved_pads.size());
        for (const auto& pad : resolved_pads) {
            resolved_indices.push_back(pad.sdl_index);
        }
    }
    virtual_joypad_index_ = 0;
    if (config.virtual_joypad_index.has_value()) {
        virtual_joypad_index_ = *config.virtual_joypad_index;
    } else if (!resolved_indices.empty()) {
        virtual_joypad_index_ = resolved_indices.front();
    }

    if (launch_config.standalone || system_key_ == "switch") {
        const auto runtime = resolve_switch_runtime();
        if (!runtime.has_value()) {
            const auto message = switch_runtime_unavailable_message();
            send_error_to_session_clients(plan, message);
            throw std::runtime_error(message);
        }
        launch_config.standalone = true;
        launch_config.core_path = runtime->path;
        launch_config.standalone_args_before_content = runtime->args_before_content;
        switch_backend = make_switch_backend(*runtime);
    }

    if (launch_config.standalone) {
        if (!switch_backend) {
            throw std::runtime_error("Switch standalone launch missing backend");
        }
        std::vector<ClientHello> client_hellos;
        client_hellos.reserve(plan.clients.size());
        for (const auto& client : plan.clients) {
            client_hellos.push_back(client.hello);
        }
        const auto profile_name = resolve_switch_profile_display_name(
            save_profile_.username, plan.host_hello, client_hellos);
        auto switch_prep = switch_backend->prepare(
            launch_config,
            SwitchBackendPrepContext{
                save_profile_,
                launch_plan.players,
                config.verbose,
                product_id_base,
                config.ignore_controller.value_or(""),
                config.graphics_api,
                virtualgl_capture,
                gamescope_capture_,
                config.resolution.switch_scale,
                &resolved_gpu,
                profile_name,
                std::move(resolved_pads),
            });
        resolved_pads = std::move(switch_prep.resolved_pads);
        switch_backend->assign_launch_env_profile(launch_env_request, switch_prep);
        log_switch_backend_prep(
            *switch_backend,
            launch_env_request,
            switch_prep,
            config.resolution.switch_scale,
            resolved_gpu,
            slot);
        if (switch_backend->enable_soft_keyboard()) {
            ensure_soft_keyboard(
                plan.soft_keyboard,
                profile_name,
                "What is your name?",
                capture_display);
        }
    } else {
        RetroArchOverrideParams override_params;
        override_params.first_virtual_joypad_index = virtual_joypad_index_;
        override_params.identities = &launch_plan.virtual_identities;
        override_params.joypad_driver = config.retroarch_joypad_driver;
        override_params.players = launch_plan.players;
        override_params.save_profile = &save_profile_;
        override_params.realtime_pacing = config.audio || config.video;
        override_params.capture_fullscreen = capture_fullscreen && use_virtual_capture_;
        override_params.capture_resolution = config.video_resolution;
        override_params.vulkan_gpu_index =
            (!use_virtual_capture_ && resolved_gpu.has_value()) ? resolved_gpu->vulkan_index : -1;
        override_params.system_key = system_key_;
        override_params.core_path = launch_config.core_path;
        override_params.resolution_scale = config.resolution.retroarch_scale;
        override_params.slot_index = slot;
        override_params.network_cmd_port = plan.retroarch_netcmd_port;
        apply_retroarch_override(launch_config, override_params);
    }

    apply_capture_and_launch_environment(
        launch_config,
        capture,
        config,
        gamescope_vk_device,
        resolved_gpu,
        launch_env_request);

    if (config.audio) {
        park_session_game_audio(config_.streaming_audio, slot);
    }

    input_router_ = std::make_unique<InputRouter>(*gamepads_, keyboard_.get());
    input_router_->set_seat_assignment(launch_plan.seats);

    media_server_ = start_host_media_server_if_needed(HostMediaStartRequest{
        config,
        capture_display,
        display_backend,
        nvenc_cuda_device_id,
        media_config,
        media_destinations,
        media_streams,
    });
    if (media_server_ != nullptr) {
        media_index_ = media_destinations.size();
    }

    const auto slot_prefix = "session slot " + std::to_string(slot) + ": ";
    plug_virtual_keyboard_with_retry(
        *keyboard_,
        use_virtual_capture_,
        gamescope_capture_,
        slot_prefix);

    for (const auto& stream : media_streams) {
        if (stream.client_id == HostClientId) {
            continue;
        }
        if (!stream.endpoint.video_uri.empty() || !stream.endpoint.audio_uri.empty()) {
            send_media_endpoint_to_client(plan, stream.client_id, stream.endpoint);
        }
    }

    send_session_starting_to_clients(plan);

    if (media_server_ != nullptr) {
        std::uint16_t capture_w = 1920;
        std::uint16_t capture_h = 1080;
        parse_video_resolution(config.video_resolution, capture_w, capture_h);
        session_monitor_.emplace(
            plan,
            *input_router_,
            *media_server_,
            std::chrono::seconds(config.client_timeout_seconds),
            std::chrono::seconds(config.player_reconnect_timeout_seconds),
            config_.hub,
            capture_w,
            capture_h);
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
        media_streams,
        *session_runtime_,
        config_.streaming_audio,
        slot,
        *gamepads_,
        launch_plan.players,
        config.pulse_input,
        keyboard_.get(),
        gamescope_capture_,
        capture_display,
        slot_prefix);

    register_input_clients();

    std::optional<std::string> session_end_reason;
    const RelaunchContext relaunch_ctx{
        capture_fullscreen,
        &resolved_gpu,
        &launch_env_request,
    };
    SessionLoopCadence loop_cadence(
        local_bridge.has_value() ? &*local_bridge : nullptr,
        input_router_.get(),
        config_.streaming_audio,
        slot,
        config.audio);

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
        loop_cadence.tick();
    }

    if (!should_stop() && !session_end_reason.has_value() && !session_runtime_->emulator_running()) {
        const auto code = session_runtime_->last_exit_code().value_or(-1);
        std::ostringstream reason;
        reason << "emulator exited (code " << code << ")";
        session_end_reason = reason.str();
    }

    // The runtime destructor also stops any process it still owns; stop here so
    // teardown ordering stays predictable.
    stop_session_runtime(session_runtime_, /*reset=*/true);

    // Pull Ryujinx/Yuzu Switch saves into the shared canonical tree after exit.
    // (Launch already synced; in-session Ryujinx writes stay in bis until now.)
    sync_and_log_post_exit_switch_saves(save_profile_, slot, switch_backend.get());

    // Drop XTest before tearing down Xvfb. Otherwise Xlib's fatal I/O handler
    // calls exit(1) and kills the whole host_runner lobby ("XIO: fatal IO error
    // on X server :99" right after "session finished; lobby still accepting").
    if (input_router_ != nullptr) {
        unregister_input_clients();
        input_router_.reset();
    }
    unplug_session_keyboard(keyboard_.get());
    keyboard_.reset();

    const std::string end_reason = should_stop()
        ? "host stopped"
        : session_end_reason.value_or("session ended");
    shutdown_media_and_clients(end_reason);
}

} // namespace archstreamer
