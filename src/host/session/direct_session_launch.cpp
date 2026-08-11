#include "host/session/direct_session_launch.hpp"

#include "common/catalog_presenter.hpp"
#include "common/cli_common.hpp"
#include "common/participant_role.hpp"
#include "host/db/cadence_session_events.hpp"
#include "host/lobby/host_session_helpers.hpp"
#include "host/virtual/input_router.hpp"
#include "host/hardware/launch_environment.hpp"
#include "host/console/nds/melonds_backend.hpp"
#include "host/virtual/pad_plan.hpp"
#include "host/console/retroarch_config_writer.hpp"
#include "host/console/retroarch_resolve.hpp"
#include "host/session/emulator_backend.hpp"
#include "host/session/launch_assemble.hpp"
#include "host/session/run_helpers.hpp"
#include "host/virtual/soft_keyboard_host.hpp"
#include "host/console/switch/switch_backend.hpp"
#include "host/virtual/virtual_joypad_resolve.hpp"

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
    SessionBackendPrepareContext& backend,
    VirtualKeyboard& keyboard) {
    const auto result = prepare_session_emulator_backend(
        backend,
        SessionBackendPrepareOptions{
            0,
            backend.input.devices.capture.use_virtual_capture,
            backend.video.capture.gamescope_capture,
            true,
            false,
            DefaultRetroArchNetcmdPort,
            &keyboard,
        });
    if (result.soft_keyboard) {
        backend.input.devices.keyboard.standalone_soft_keyboard = result.soft_keyboard;
    }
}

} // namespace archstreamer
