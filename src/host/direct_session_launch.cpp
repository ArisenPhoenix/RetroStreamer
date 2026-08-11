#include "host/direct_session_launch.hpp"

#include "common/catalog_presenter.hpp"
#include "common/cli_common.hpp"
#include "common/participant_role.hpp"
#include "common/steam_art_import.hpp"
#include "host/cadence_session_events.hpp"
#include "host/host_session_helpers.hpp"
#include "host/input_router.hpp"
#include "host/launch_environment.hpp"
#include "host/nds/melonds_backend.hpp"
#include "host/pad_plan.hpp"
#include "host/retroarch_config_writer.hpp"
#include "host/retroarch_resolve.hpp"
#include "host/session_launch_assemble.hpp"
#include "host/session_run_helpers.hpp"
#include "host/soft_keyboard_host.hpp"
#include "host/standalone_emulator.hpp"
#include "host/switch/switch_backend.hpp"
#include "host/switch_save_share.hpp"
#include "host/virtual_joypad_resolve.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>

namespace archstreamer {

std::optional<SessionLaunchContext> prepare_direct_session_context(
    HostAppConfig& config,
    GameCatalog& catalog,
    const GameList& list,
    const std::optional<HostPlayerControllerIdentity>& bridge_identity) {
    const auto game_id = select_game_for_launch(list, *config.selector);
    if (!game_id.has_value()) {
        std::cerr << "Game not found: " << *config.selector << '\n';
        return std::nullopt;
    }
    const auto selected_game = game_info_for(list, *game_id);
    if (!selected_game.has_value()) {
        throw std::runtime_error("selected game is missing from game list");
    }

    if (config.username.empty()) {
        config.username = default_cli_username();
    }
    auto launch_plan = launch_plan_for_direct(
        *selected_game,
        config.session_mode,
        config.players,
        config.username,
        bridge_identity);

    if (launch_plan.save_username.empty()) {
        launch_plan.save_username = default_cli_username();
    }
    if (!valid_username(launch_plan.save_username)) {
        throw std::runtime_error("save username must be 1-64 characters and contain only letters, numbers, underscores, or hyphens");
    }
    if (launch_plan.virtual_identities.size() < launch_plan.players) {
        launch_plan.virtual_identities.resize(launch_plan.players);
    }

    return prepare_session_launch_context(
        config,
        catalog,
        std::move(launch_plan));
}



void print_direct_launch_summary(
    const HostAppConfig& config,
    const HostLaunchPlan& launch_plan,
    const SaveProfile& save_profile,
    const RetroArchLaunchConfig& launch_config,
    const std::vector<MediaClientStream>& media_streams,
    const std::string& capture_display) {
    std::cout
        << "Selected game: " << launch_plan.game_id
        << "\nRetroArch: " << launch_config.retroarch_path
        << "\nCore:      " << launch_config.core_path
        << "\nContent:   " << launch_config.content_path
        << "\nMode:      " << session_mode_name(launch_plan.session_mode)
        << "\nPlayers:   " << static_cast<int>(launch_plan.players)
        << "\nHostRole:  " << participant_role_name(config.host_role)
        << "\nJoypad:    " << config.retroarch_joypad_driver
        << "\nUser:      " << save_profile.username
        << "\nSaves:     " << save_profile.savefile_directory
        << "\nStates:    " << save_profile.savestate_directory
        << '\n';
    for (const auto& stream : media_streams) {
        if (!stream.endpoint.video_uri.empty()) {
            std::cout
                << "Video:     client " << static_cast<int>(stream.client_id)
                << " " << stream.endpoint.video_uri
                << " from display " << capture_display
                << " at " << config.video_resolution << '\n';
        }
        if (!stream.endpoint.audio_uri.empty()) {
            std::cout
                << "Audio:     client " << static_cast<int>(stream.client_id)
                << " " << stream.endpoint.audio_uri;
            if (!config.audio_source.empty()) {
                std::cout << " from " << config.audio_source;
            }
            std::cout
                << " via "
                << (config.audio_backend == AudioCaptureBackend::PipeWire
                        ? "pipewire"
                        : config.audio_backend == AudioCaptureBackend::Wasapi ? "wasapi"
                                                                             : "pulse")
                << '\n';
        }
    }
    for (RetroArchPort port = 0; port < launch_plan.players; ++port) {
        const auto identity = identity_for_port(launch_plan.virtual_identities, port);
        std::cout
            << "Virtual:   P" << static_cast<int>(port) + 1
            << " " << identity.name << " P" << static_cast<int>(port) + 1
            << " " << hex_vid_pid(
                identity.vendor_id,
                static_cast<std::uint16_t>(identity.product_id + port))
            << '\n';
    }
    if (config.ignore_controller.has_value()) {
        std::cout << "Ignoring:  " << *config.ignore_controller << '\n';
    }
}

std::string direct_emulator_command(
    const RetroArchLaunchConfig& launch_config,
    const ResolvedRetroArch& resolved_retroarch) {
    std::string command;
    for (const auto& arg : launch_config.command_prefix) {
        if (!command.empty()) {
            command.push_back(' ');
        }
        command += arg;
    }
    if (launch_config.standalone) {
        if (!command.empty()) {
            command.push_back(' ');
        }
        command += launch_config.core_path.string();
        for (const auto& arg : launch_config.standalone_args_before_content) {
            command.push_back(' ');
            command += arg;
        }
        for (const auto& arg : launch_config.extra_args) {
            command.push_back(' ');
            command += arg;
        }
        command.push_back(' ');
        command += launch_config.content_path.string();
        return command;
    }

    if (command.empty()) {
        command = resolved_retroarch.display_path;
    }
    for (const auto& arg : launch_config.extra_args) {
        command.push_back(' ');
        command += arg;
    }
    command += " -L ";
    command += launch_config.core_path.string();
    command.push_back(' ');
    command += launch_config.content_path.string();
    return command;
}

void log_direct_emulator_command(
    const RetroArchLaunchConfig& launch_config,
    const ResolvedRetroArch& resolved_retroarch) {
    std::cout
        << (launch_config.standalone ? "Launching standalone emulator..." : "Launching RetroArch...")
        << "\nCommand: " << direct_emulator_command(launch_config, resolved_retroarch) << '\n';
}

void print_input_seats(const SeatAssignment& seats) {
    std::cout << "Input seats: " << seats.seats.size() << '\n';
    for (const auto& seat : seats.seats) {
        std::cout
            << "  client " << static_cast<int>(seat.client_id)
            << " local P" << static_cast<int>(seat.local_player) + 1
            << " -> RetroArch P" << static_cast<int>(seat.retroarch_port) + 1 << '\n';
    }
}

CadenceSessionTracker begin_direct_cadence_session(
    const HostLaunchPlan& launch_plan,
    const HostAppConfig& config,
    const SessionRuntime& session_runtime) {
    CadenceSessionTracker cadence_tracker;
    std::ostringstream detail;
    detail << session_mode_name(launch_plan.session_mode) << " direct";
    const std::string sink = StreamingAudioSink::kName;
    cadence_tracker.begin(
        0,
        launch_plan.save_username,
        launch_plan.game_id,
        {},
        detail.str(),
        config.virtual_display,
        config.video_port,
        config.audio_port,
        DefaultRetroArchNetcmdPort,
        sink,
        sink,
        0xa517);
    if (const auto pid = session_runtime.emulator().process_id(); pid.has_value()) {
        cadence_tracker.claim_emulator_pid(*pid);
    }
    record_session_started(
        0,
        launch_plan.save_username,
        launch_plan.game_id,
        detail.str(),
        cadence_tracker.session_id());
    return cadence_tracker;
}

void end_direct_cadence_session(
    CadenceSessionTracker& cadence_tracker,
    const HostLaunchPlan& launch_plan,
    std::string_view end_reason) {
    record_session_ended(
        0,
        launch_plan.save_username,
        launch_plan.game_id,
        end_reason,
        cadence_tracker.session_id());
    cadence_tracker.end(end_reason);
}

EmulatorLaunchEnvRequest build_launch_env_request(
    const HostAppConfig& config,
    const CapturePlan& capture,
    bool host_plays_locally) {
    EmulatorLaunchEnvRequest launch_env_request;
    launch_env_request.host_plays_locally = host_plays_locally;
    launch_env_request.stream_media = config.audio || config.video;
    launch_env_request.stream_audio = config.audio;
    launch_env_request.audio_source = config.audio_source;
    launch_env_request.ignore_devices = *config.ignore_controller;
    launch_env_request.use_virtual_capture = capture.use_virtual_capture;
    launch_env_request.gamescope_capture = capture.gamescope_capture;
    launch_env_request.virtualgl_capture = capture.virtualgl_capture;
    launch_env_request.capture_display = capture.capture_display;
    launch_env_request.xtest_display = launch_env_request.gamescope_capture
        ? gamescope_xtest_display_for_slot(0)
        : launch_env_request.capture_display;
    // Direct CLI path has no Lobby session id — mint one for the XTest lease map.
    const std::string session_id = "direct-" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    launch_env_request.session_id = session_id;
    return launch_env_request;
}


SessionGpuSelection prepare_direct_gpu(
    const HostAppConfig& config,
    EmulatorLaunchEnvRequest& launch_env_request,
    const CapturePlan& capture) {
    auto gpu_selection = resolve_session_gpu_selection(
        config,
        capture,
        {},
        /*detailed_logging=*/true);
    apply_session_gpu_to_launch_request(launch_env_request, gpu_selection);
    return gpu_selection;
}

SessionLaunchEnvironment prepare_direct_launch_environment(
    HostAppConfig& config,
    RetroArchLaunchConfig& launch_config,
    bool host_plays_locally) {
    auto capture = resolve_capture_plan(config, launch_config);
    auto request = build_launch_env_request(config, capture, host_plays_locally);
    auto gpu = prepare_direct_gpu(config, request, capture);
    return SessionLaunchEnvironment{
        std::move(capture),
        std::move(request),
        std::move(gpu),
    };
}

void prepare_direct_backend(
    const HostAppConfig& config,
    const CapturePlan& capture,
    SessionLaunchContext& context,
    SessionDevicePlan& devices,
    VirtualKeyboard& keyboard,
    EmulatorLaunchEnvRequest& launch_env_request,
    SessionBackendState& backends) {
    auto& launch_plan = context.launch_plan;
    auto& assets = context.assets;
    auto& launch_config = assets.launch_config;

    if (backends.system_key == "switch") {
        const auto runtime = resolve_switch_runtime();
        if (!runtime.has_value()) {
            throw std::runtime_error(switch_runtime_unavailable_message());
        }
        launch_config.standalone = true;
        launch_config.core_path = runtime->path;
        launch_config.standalone_args_before_content = runtime->args_before_content;
        backends.switch_backend = make_switch_backend(*runtime);
    } else if (backends.system_key == "nds" && melonds_runtime_available()) {
        const auto runtime = resolve_melonds_runtime();
        if (!runtime.has_value()) {
            throw std::runtime_error(melonds_unavailable_message());
        }
        launch_config.standalone = true;
        launch_config.core_path = runtime->path;
        launch_config.standalone_args_before_content = runtime->args_before_content;
        backends.melonds_backend = make_melonds_backend();
    } else if (launch_config.standalone) {
        throw std::runtime_error(
            "standalone launch requested for unsupported system_key=" + backends.system_key);
    }

    if (backends.switch_backend) {
        keyboard.set_switch_style_hotkeys(true);
        const auto profile_name =
            preferred_steam_or_username_display_name(assets.save_profile.username);
        const auto switch_content_stem = !assets.content.catalog_content_path.empty()
            ? assets.content.catalog_content_path.stem().string()
            : launch_config.content_path.stem().string();
        auto switch_title_id = assets.content.m3m_title_id;
        if (switch_title_id.empty()) {
            switch_title_id = resolve_switch_title_id_for_catalog(
                assets.save_profile, switch_content_stem, launch_config.content_path);
        }
        auto switch_prep = backends.switch_backend->prepare(
            launch_config,
            SwitchBackendPrepContext{
                assets.save_profile,
                launch_plan.players,
                config.verbose,
                /*product_id_base=*/0,
                config.ignore_controller.value_or(""),
                config.graphics_api,
                capture.virtualgl_capture,
                capture.gamescope_capture,
                config.resolution.switch_scale,
                /*prefer_handheld_mode=*/false,
                &devices.resolved_gpu,
                profile_name,
                std::move(devices.resolved_pads),
                /*slot_index=*/0,
                launch_plan.game_id,
                switch_content_stem,
                switch_title_id,
            });
        devices.resolved_pads = std::move(switch_prep.resolved_pads);
        backends.switch_backend->assign_launch_env_profile(launch_env_request, switch_prep);
        log_switch_backend_prep(
            *backends.switch_backend,
            launch_env_request,
            switch_prep,
            config.resolution.switch_scale,
            devices.resolved_gpu);
        backends.switch_launch_content_stem = switch_content_stem;
        backends.switch_launch_title_id = switch_title_id;
        if (backends.switch_backend->enable_soft_keyboard()) {
            if (!devices.standalone_soft_keyboard) {
                devices.standalone_soft_keyboard = std::make_shared<SoftKeyboardHostBridge>();
            }
            devices.soft_keyboard_fallback = profile_name;
            devices.arm_soft_keyboard = true;
        }
    } else if (backends.melonds_backend) {
        const auto profile_name =
            preferred_steam_or_username_display_name(assets.save_profile.username);
        auto melonds_prep = backends.melonds_backend->prepare(
            launch_config,
            MelonDsBackendPrepContext{
                assets.save_profile,
                launch_plan.players,
                config.verbose,
                /*product_id_base=*/0,
                config.ignore_controller.value_or(""),
                capture.virtualgl_capture,
                capture.gamescope_capture,
                /*slot_index=*/0,
                profile_name,
                DisplayLayoutPreference::Auto,
                std::move(devices.resolved_pads),
            });
        devices.resolved_pads = std::move(melonds_prep.resolved_pads);
        backends.melonds_backend->assign_launch_env_profile(launch_env_request, melonds_prep);
        log_melonds_backend_prep(*backends.melonds_backend, launch_env_request, melonds_prep);
    } else {
        RetroArchOverrideParams override_params;
        override_params.first_virtual_joypad_index = devices.virtual_joypad_index;
        override_params.identities = &launch_plan.virtual_identities;
        override_params.joypad_driver = config.retroarch_joypad_driver;
        override_params.players = launch_plan.players;
        override_params.save_profile = &assets.save_profile;
        override_params.realtime_pacing = config.audio || config.video;
        override_params.capture_fullscreen = devices.capture_fullscreen && devices.use_virtual_capture;
        override_params.capture_resolution = config.video_resolution;
        override_params.vulkan_gpu_index =
            (!devices.use_virtual_capture && devices.resolved_gpu.has_value()) ? devices.resolved_gpu->vulkan_index : -1;
        override_params.system_key = backends.system_key;
        override_params.core_path = launch_config.core_path;
        override_params.resolution_scale = config.resolution.retroarch_scale;
        const auto runtime_override = apply_retroarch_override(launch_config, override_params);
        std::cout
            << "RetroArch config: " << runtime_override
            << "\nVirtual joypad index: " << devices.virtual_joypad_index
            << " (driver=" << config.retroarch_joypad_driver << ")\n";
        {
            const int scale = std::clamp(config.resolution.retroarch_scale, 1, 6);
            std::cout << "RetroArch resolution: " << scale << "x native"
                      << " (known cores via .opt)\n";
        }
        if (!backends.system_key.empty()) {
            std::cout << "Face buttons: system=" << backends.system_key
                      << " (" << face_button_map_name(backends.system_key) << ")\n";
        }
        launch_env_request.pad_plan = devices.shared_pad_plan;
        log_pad_plan(devices.shared_pad_plan);
    }
}

} // namespace archstreamer
