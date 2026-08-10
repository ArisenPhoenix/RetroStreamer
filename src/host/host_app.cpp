#include "common/catalog_presenter.hpp"
#include "common/cli_common.hpp"
#include "common/ds_touch_mapping.hpp"
#include "common/participant_role.hpp"
#include "common/platform/default_platform.hpp"
#include "common/platform/process_utils.hpp"
#include "common/steam_art_import.hpp"
#include "client/controller_backend.hpp"
#include "host/capture_platform.hpp"
#include "host/emulator_orphan_reaper.hpp"
#include "host/game_catalog.hpp"
#include "archstreamer/runtime_cadence/cadence.hpp"
#include "host/cadence_session_events.hpp"
#include "host/cadence_session_tracker.hpp"
#include "host/game_catalog_scanner.hpp"
#include "host/game_meta_store.hpp"
#include "host/gpu_select.hpp"
#include "host/host_app.hpp"
#include "host/host_app_config.hpp"
#include "host/host_concurrent_lobby.hpp"
#include "host/active_session_slot.hpp"
#include "host/streaming_audio_sink.hpp"
#include "host/host_launch_planner.hpp"
#include "host/host_session_helpers.hpp"
#include "host/input_router.hpp"
#include "host/launch_environment.hpp"
#include "host/virtual_display.hpp"
#include "host/virtual_keyboard.hpp"
#include "host/soft_keyboard_host.hpp"
#include "host/local_controller_bridge.hpp"
#include "host/network_input_receiver.hpp"
#include "host/platform/default_host_platform.hpp"
#include "host/retroarch_config_writer.hpp"
#include "host/retroarch_resolve.hpp"
#include "host/save_profile.hpp"
#include "host/session_launch_assemble.hpp"
#include "host/session_run_helpers.hpp"
#include "host/standalone_emulator.hpp"
#include "host/switch_save_share.hpp"
#include "host/session_lobby.hpp"
#include "host/session_runtime.hpp"
#include "host/switch/switch_backend.hpp"
#include "host/nds/melonds_backend.hpp"
#include "host/pad_plan.hpp"
#include "host/virtual_joypad_resolve.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace archstreamer {

HostApp::HostApp(HostAppConfig config)
    : config_(std::move(config)) {
}

void HostApp::apply_host_player_media_policy(HostAppConfig& config, bool host_plays_locally) {
    // Host Player is local RetroArch on the real display/speakers. Streaming the
    // same session captures the playback sink monitor (or fights virtio-evdev for
    // the pad) and sounds/feels broken — force media off regardless of CLI/GUI.
    if (host_plays_locally && (config.audio || config.video)) {
        std::cout
            << "Host Player: disabling stream video/audio for local play "
            << "(use Host Viewer to stream).\n";
        config.audio = false;
        config.video = false;
    }
}

void HostApp::prepare_streaming_audio_source(HostAppConfig& config, StreamingAudioSink& sink) {
    if (!config.audio || !config.audio_source.empty()) {
        return;
    }
    // Concurrent lobby uses archstreamer-0..N per session slot. Creating the
    // legacy "archstreamer" sink here only clutters the system mixer.
    if (config.control_port.has_value()) {
        sink.prune_unused(
            static_cast<int>(clamp_max_session_slots(config.clients)),
            /*keep_legacy=*/false);
        sink.restore_default_sink();
        std::cout
            << "Audio capture: per-slot null sinks (archstreamer-0…); "
            << "host speakers stay quiet unless Watch stream locally\n";
        return;
    }
    try {
        config.audio_source = sink.monitor_source();
        std::cout
            << "Audio capture: " << config.audio_source
            << " (null sink; host speakers stay quiet unless Watch stream locally)\n";
    } catch (const std::exception& error) {
        config.audio_source = StreamingAudioSink::default_monitor_source();
        std::cerr << "Warning: " << error.what() << '\n';
        if (config.audio_source.empty()) {
            std::cerr
                << "Warning: could not determine audio monitor source; "
                << "audio capture will use the audio server default source.\n";
        } else {
            std::cerr
                << "Warning: falling back to default sink monitor "
                << config.audio_source
                << " (host may hear game audio locally).\n";
        }
    }
}

std::optional<ControllerDevice> HostApp::resolve_bridge_device(const HostAppConfig& config) {
    if (config.host_role == ParticipantRole::Viewer && config.bridge_controller_index.has_value()) {
        throw std::runtime_error("--bridge-controller cannot be used with --host-role viewer");
    }
    if (config.host_role == ParticipantRole::Player && !config.bridge_controller_index.has_value()) {
        throw std::runtime_error(
            "--host-role player requires --bridge-controller "
            "(or use --host-role viewer for a dedicated streaming host)");
    }
    if (config.host_role == ParticipantRole::Player && config.bridge_controller_index.has_value()) {
        return local_bridge_device_for(*config.bridge_controller_index);
    }
    return std::nullopt;
}

namespace {

void append_direct_controller_ignore_list(
    HostAppConfig& config,
    const std::optional<ControllerDevice>& bridge_device) {
    if (bridge_device.has_value() && !config.ignore_controller.has_value()) {
        if (bridge_device->vendor_id != 0 && bridge_device->product_id != 0) {
            config.ignore_controller = hex_vid_pid(bridge_device->vendor_id, bridge_device->product_id);
        }
    }

    // Blacklist every physical pad currently attached so RetroArch is less likely to
    // bind P1 to a host controller. Virtual ArchStreamer pads are created after this.
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
        std::cerr << "Warning: host controller scan for ignore list failed: " << error.what() << '\n';
    }

    auto ignore_devices = config.ignore_controller.value_or("");
    const char* steam_input = "0x28de/0x11ff,0x28de/0x1205,0x28de/0x1201";
    if (ignore_devices.empty()) {
        ignore_devices = steam_input;
    } else {
        ignore_devices = ignore_devices + "," + steam_input;
    }
    config.ignore_controller = ignore_devices;
    if (config.retroarch_joypad_driver != "sdl2") {
        std::cerr
            << "Warning: SDL_GAMECONTROLLER_IGNORE_DEVICES only affects RetroArch when "
            << "--retroarch-joypad-driver is sdl2.\n";
    }
}

struct DirectGpuSelection {
    std::optional<GpuDevice> resolved_encode;
    std::optional<GpuDevice> resolved_gpu;
    std::string gamescope_vk_device;
    int nvenc_cuda_device_id = -1;
};

DirectGpuSelection resolve_direct_gpu_selection(
    const HostAppConfig& config,
    bool use_virtual_capture,
    bool virtualgl_capture,
    bool gamescope_capture) {
    DirectGpuSelection selection;
    selection.resolved_encode = resolve_render_gpu(config.encode_gpu);
    selection.resolved_gpu = resolve_render_gpu(effective_render_gpu_selection(config));
    if (selection.resolved_encode.has_value() && selection.resolved_encode->nvidia_index >= 0) {
        selection.nvenc_cuda_device_id = selection.resolved_encode->nvidia_index;
    }
    if (selection.resolved_gpu.has_value()) {
        const bool same_as_encode =
            selection.resolved_encode.has_value() &&
            selection.resolved_encode->id == selection.resolved_gpu->id;
        if (same_as_encode) {
            std::cout
                << "GPU: " << selection.resolved_gpu->name
                << " [" << selection.resolved_gpu->id << "] (encode+render)";
        } else {
            if (selection.resolved_encode.has_value()) {
                std::cout
                    << "Encode GPU: " << selection.resolved_encode->name
                    << " [" << selection.resolved_encode->id << "]";
                if (selection.resolved_encode->nvidia_index >= 0) {
                    std::cout << " nvidia_index=" << selection.resolved_encode->nvidia_index;
                }
                std::cout << '\n';
            }
            std::cout
                << "Render GPU: " << selection.resolved_gpu->name
                << " [" << selection.resolved_gpu->id << "]";
            if (config.separate_render_gpu) {
                std::cout << " (separate from encode)";
            }
        }
        if (selection.resolved_gpu->vulkan_index >= 0) {
            std::cout << " vulkan_index=" << selection.resolved_gpu->vulkan_index;
        }
        if (!selection.resolved_gpu->prime_provider.empty()) {
            std::cout << " prime=" << selection.resolved_gpu->prime_provider;
        }
        std::cout << '\n';
        if (const auto vd = pci_vendor_device_id(selection.resolved_gpu->pci_bus); vd.has_value()) {
            selection.gamescope_vk_device = *vd;
        }
        if (selection.resolved_gpu->prime_provider.empty() &&
            selection.resolved_gpu->nvidia_index >= 0) {
            std::cerr
                << "Warning: NVIDIA GPU selected but no PRIME provider was mapped "
                << "(is DISPLAY set when scanning GPUs?). Capture GL may use llvmpipe.\n";
        }
        if (use_virtual_capture && !virtualgl_capture && !gamescope_capture &&
            selection.resolved_gpu->nvidia_index > 0) {
            std::cerr
                << "Warning: streamed OpenGL on plain Xvfb cannot select NVIDIA GPU index "
                << selection.resolved_gpu->nvidia_index
                << " (always uses nvidia:0). Install VirtualGL (vglrun) so Host GPU works.\n";
        }
    } else if (selection.resolved_encode.has_value()) {
        std::cout
            << "Encode GPU: " << selection.resolved_encode->name
            << " [" << selection.resolved_encode->id << "]";
        if (selection.resolved_encode->nvidia_index >= 0) {
            std::cout << " nvidia_index=" << selection.resolved_encode->nvidia_index;
        }
        std::cout << '\n';
    }
    if (selection.gamescope_vk_device.empty()) {
        // Prefer RTX 3060 then 1660 Ti on this host when auto-detect fails.
        selection.gamescope_vk_device = "10de:2504";
    }
    return selection;
}

struct DirectSessionTarget {
    HostLaunchPlan launch_plan;
    SaveProfile save_profile;
    RetroArchLaunchConfig launch_config;
    ResolvedRetroArch resolved_retroarch;
    std::string system_key;
    std::filesystem::path catalog_content_path;
    std::string m3m_title_id;
};

std::optional<DirectSessionTarget> prepare_direct_session_target(
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

    auto save_profile = prepare_save_profile(config.save_root, launch_plan.save_username);
    auto launch_config = catalog.launch_config_for(launch_plan.game_id);
    const auto resolved_retroarch = resolve_retroarch();

    std::string system_key;
    std::filesystem::path catalog_content_path;
    std::string m3m_title_id;
    if (const auto hosted = catalog.find_hosted(launch_plan.game_id); hosted.has_value()) {
        system_key = hosted->get().info.system_key;
        catalog_content_path = hosted->get().content_path;
        m3m_title_id = hosted->get().m3m_title_id;
    } else if (const auto info = catalog.find(launch_plan.game_id); info.has_value()) {
        system_key = info->system_key;
    }
    if (!launch_config.standalone && system_key == "ps2") {
        std::cout
            << "PS2 memcards: " << user_ps2_memcard_directory(save_profile) << '\n';
    }
    // LRPS2/PCSX ReARMed stall badly under RetroArch's sdl2 joypad poll. Keep udev
    // for PlayStation even if a caller/GUI asked for sdl2.
#if !defined(_WIN32)
    if (!launch_config.standalone &&
        (system_key == "ps1" || system_key == "ps2" || system_key == "psp") &&
        config.retroarch_joypad_driver == "sdl2") {
        std::cout
            << "Note: forcing joypad driver udev for " << system_key
            << " (sdl2 stalls PlayStation cores).\n";
        config.retroarch_joypad_driver = "udev";
    }
#endif
    if (!launch_config.standalone) {
        launch_config.retroarch_path = resolved_retroarch.display_path;
        launch_config.command_prefix = resolved_retroarch.argv_prefix;
    }
    // Avoid -f on the host's real Wayland session (can exit immediately). When video
    // streams from a virtual display, force fullscreen via the override config instead.
    if (config.verbose && !launch_config.standalone) {
        launch_config.extra_args.insert(launch_config.extra_args.begin(), "--verbose");
    }

    return DirectSessionTarget{
        std::move(launch_plan),
        std::move(save_profile),
        std::move(launch_config),
        resolved_retroarch,
        std::move(system_key),
        std::move(catalog_content_path),
        std::move(m3m_title_id),
    };
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

std::unique_ptr<MelonDsCtrlClient> configure_direct_input_router(
    InputRouter& input_router,
    VirtualKeyboard& keyboard,
    const HostLaunchPlan& launch_plan,
    const std::unique_ptr<SwitchBackend>& switch_backend,
    const std::unique_ptr<MelonDsBackend>& melonds_backend) {
    input_router.set_seat_assignment(launch_plan.seats);
    if (switch_backend) {
        input_router.set_emulator_backend(EmulatorControlBackend::Ryujinx);
    } else if (melonds_backend != nullptr) {
        input_router.set_emulator_backend(EmulatorControlBackend::MelonDS);
    } else {
        input_router.set_emulator_backend(EmulatorControlBackend::RetroArch);
    }

    auto melonds_touch_ctrl = std::unique_ptr<MelonDsCtrlClient>{};
    if (melonds_backend != nullptr && melonds_backend->profile() != nullptr) {
        const auto& ctrl_name = melonds_backend->profile()->ctrl_server_name;
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


struct DirectPadSelection {
    std::vector<std::size_t> resolved_indices;
    std::vector<ArchStreamerSdlPad> resolved_pads;
    std::size_t virtual_joypad_index = 0;
    PadPlan shared_pad_plan;
};

DirectPadSelection resolve_direct_pad_selection(
    const HostAppConfig& config,
    const HostLaunchPlan& launch_plan) {
    // Resolve joypad indices after uinput pads appear. Prefer discovered index so
    // RetroArch binds the ArchStreamer pad even when host controllers remain visible.
    // udev and sdl2 enumerate pads differently — match the driver RetroArch will use.
    DirectPadSelection selection;
    const bool use_udev = config.retroarch_joypad_driver == "udev";
    if (config.verbose && use_udev) {
        std::cout << "udev joysticks (ArchStreamer hunt):\n";
    }
    selection.shared_pad_plan = resolve_shared_pad_plan(
        launch_plan.players,
        config.ignore_controller.value_or(""),
        config.verbose,
        /*product_id_base=*/0,
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
        std::cout << "Using explicit --virtual-joypad-index " << selection.virtual_joypad_index << '\n';
    } else if (!selection.resolved_indices.empty()) {
        selection.virtual_joypad_index = selection.resolved_indices.front();
        if (config.verbose) {
            std::cout
                << "Resolved virtual joypad index " << selection.virtual_joypad_index
                << " (driver=" << config.retroarch_joypad_driver << ")\n";
        }
    } else {
        std::cerr
            << "Warning: ArchStreamer virtual pads not visible to "
            << config.retroarch_joypad_driver
            << " yet; defaulting RetroArch joypad index to 0.\n";
    }
    return selection;
}


DirectGpuSelection prepare_direct_gpu(
    const HostAppConfig& config,
    EmulatorLaunchEnvRequest& launch_env_request,
    const CapturePlan& capture) {
    auto gpu_selection = resolve_direct_gpu_selection(
        config,
        capture.use_virtual_capture,
        capture.virtualgl_capture,
        capture.gamescope_capture);
    if (gpu_selection.resolved_gpu.has_value()) {
        // PRIME offload for NVIDIA. On plain Xvfb the provider name is ignored (no RandR
        // providers); VirtualGL uses the real display's GLX where G0/G1 selection works.
        launch_env_request.render_gpu = *gpu_selection.resolved_gpu;
    }
    return gpu_selection;
}

struct DirectLaunchEnvironment {
    CapturePlan capture;
    EmulatorLaunchEnvRequest request;
    DirectGpuSelection gpu;
};

DirectLaunchEnvironment prepare_direct_launch_environment(
    HostAppConfig& config,
    RetroArchLaunchConfig& launch_config,
    bool host_plays_locally) {
    auto capture = resolve_capture_plan(config, launch_config);
    auto request = build_launch_env_request(config, capture, host_plays_locally);
    auto gpu = prepare_direct_gpu(config, request, capture);
    return DirectLaunchEnvironment{
        std::move(capture),
        std::move(request),
        std::move(gpu),
    };
}

struct DirectMediaPlan {
    HostMediaPlanConfig config;
    std::vector<HostMediaDestination> destinations;
    std::vector<MediaClientStream> streams;
};

DirectMediaPlan build_direct_media_plan(const HostAppConfig& config) {
    DirectMediaPlan plan;
    plan.config = media_plan_config_for(config);
    if (config.video || config.audio) {
        plan.destinations = media_destinations_for_host(plan.config);
        plan.streams = media_streams_for_dry_run(plan.config, plan.destinations);
    }
    return plan;
}

struct DirectBackendState {
    std::unique_ptr<SwitchBackend> switch_backend;
    std::unique_ptr<MelonDsBackend> melonds_backend;
    std::string switch_launch_content_stem;
    std::string switch_launch_title_id;
    std::string system_key;
};

struct DirectDevicePlan {
    PadPlan shared_pad_plan;
    std::vector<std::size_t> resolved_indices;
    std::vector<ArchStreamerSdlPad> resolved_pads;
    std::size_t virtual_joypad_index = 0;
    std::string soft_keyboard_fallback;
    std::shared_ptr<SoftKeyboardHostBridge> standalone_soft_keyboard;
    std::optional<GpuDevice> resolved_gpu;
    bool arm_soft_keyboard = false;
    bool use_virtual_capture = false;
    bool capture_fullscreen = false;
    std::string capture_display;
    VirtualDisplayBackend display_backend = VirtualDisplayBackend::None;
};

DirectDevicePlan resolve_direct_device_plan(
    const HostLaunchPlan& launch_plan,
    const HostAppConfig& config,
    const CapturePlan& capture) {
    auto pads = resolve_direct_pad_selection(config, launch_plan);
    DirectDevicePlan devices;
    devices.shared_pad_plan = std::move(pads.shared_pad_plan);
    devices.resolved_indices = std::move(pads.resolved_indices);
    devices.resolved_pads = std::move(pads.resolved_pads);
    devices.virtual_joypad_index = pads.virtual_joypad_index;
    devices.use_virtual_capture = capture.use_virtual_capture;
    devices.capture_fullscreen = capture.capture_fullscreen;
    devices.capture_display = capture.capture_display;
    devices.display_backend = capture.display_backend;
    return devices;
}

void prepare_direct_backend(
    const HostAppConfig& config,
    RetroArchLaunchConfig& launch_config,
    const CapturePlan& capture,
    const DirectSessionTarget& target,
    HostLaunchPlan& launch_plan,
    DirectDevicePlan& devices,
    VirtualKeyboard& keyboard,
    EmulatorLaunchEnvRequest& launch_env_request,
    DirectBackendState& backends) {
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
            preferred_steam_or_username_display_name(target.save_profile.username);
        const auto switch_content_stem = !target.catalog_content_path.empty()
            ? target.catalog_content_path.stem().string()
            : launch_config.content_path.stem().string();
        auto switch_title_id = target.m3m_title_id;
        if (switch_title_id.empty()) {
            switch_title_id = resolve_switch_title_id_for_catalog(
                target.save_profile, switch_content_stem, launch_config.content_path);
        }
        auto switch_prep = backends.switch_backend->prepare(
            launch_config,
            SwitchBackendPrepContext{
                target.save_profile,
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
            preferred_steam_or_username_display_name(target.save_profile.username);
        auto melonds_prep = backends.melonds_backend->prepare(
            launch_config,
            MelonDsBackendPrepContext{
                target.save_profile,
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
        override_params.save_profile = &target.save_profile;
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



} // namespace


int HostApp::run_lobby_sessions(
    HostAppConfig config,
    GameCatalog& catalog,
    const GameList& list,
    StreamingAudioSink& streaming_audio,
    std::optional<ControllerDevice> bridge_device,
    const std::function<bool()>& should_stop) {
    return run_concurrent_session_host(
        std::move(config),
        catalog,
        list,
        streaming_audio,
        bridge_device,
        should_stop);
}

int HostApp::run_direct_session(
    HostAppConfig config,
    GameCatalog& catalog,
    const GameList& list,
    StreamingAudioSink& streaming_audio,
    std::optional<ControllerDevice> bridge_device,
    std::optional<HostPlayerControllerIdentity> bridge_identity,
    bool host_plays_locally,
    const std::function<bool()>& should_stop) {
    auto backends = DirectBackendState{};
    std::optional<std::string> session_end_reason;

    auto target_result = prepare_direct_session_target(config, catalog, list, bridge_identity);
    if (!target_result.has_value()) {
        return 1;
    }
    auto target = std::move(*target_result);
    backends.system_key = std::move(target.system_key);

    // Streaming already forced off above for Host Player.
    // Emulator child env is assembled once later (audio/input/gpu/capture/emulator).
    append_direct_controller_ignore_list(config, bridge_device);

    // Host Player keeps the real DISPLAY (and speakers). Streamed RetroArch needs a
    // virtual capture surface. Switch standalone defaults to headless gamescope on Linux;
    // Windows captures the desktop/HWND via d3d11screencapturesrc (no gamescope).
    auto launch_env = prepare_direct_launch_environment(
        config,
        target.launch_config,
        host_plays_locally);
    register_session_xtest_display(launch_env.request.session_id, launch_env.request.xtest_display);

    auto media = build_direct_media_plan(config);

    print_direct_launch_summary(
        config,
        target.launch_plan,
        target.save_profile,
        target.launch_config,
        media.streams,
        launch_env.capture.capture_display);

    if (config.dry_run) {
        return 0;
    }

    HostVirtualGamepadBus gamepads(target.launch_plan.virtual_identities);
    for (RetroArchPort port = 0; port < target.launch_plan.players; ++port) {
        gamepads.plug(port);
    }
    // Virtual keyboard targets ARCHSTREAMER_XTEST_DISPLAY for gamescope, else Xvfb capture.
    VirtualKeyboard keyboard(launch_env.request.xtest_display);
    std::this_thread::sleep_for(std::chrono::milliseconds(750));

    auto devices = resolve_direct_device_plan(target.launch_plan, config, launch_env.capture);
    devices.resolved_gpu = launch_env.gpu.resolved_gpu;

    prepare_direct_backend(
        config,
        target.launch_config,
        launch_env.capture,
        target,
        target.launch_plan,
        devices,
        keyboard,
        launch_env.request,
        backends);

    apply_capture_and_launch_environment(
        target.launch_config,
        launch_env.capture,
        config,
        launch_env.gpu.gamescope_vk_device,
        launch_env.gpu.resolved_gpu,
        launch_env.request);

    if (devices.capture_fullscreen) {
        std::cout
            << "Capture fullscreen: " << config.video_resolution
            << " on display " << devices.capture_display
            << (devices.use_virtual_capture ? " (virtual)" : " (host)") << '\n';
    }

    // Pin Viewer RetroArch to the capture null sink (speakers stay quiet unless Watch-local).
    if (config.audio) {
        park_session_game_audio(&streaming_audio);
    }

    InputRouter input_router(gamepads, &keyboard);
    auto melonds_touch_ctrl = configure_direct_input_router(
        input_router,
        keyboard,
        target.launch_plan,
        backends.switch_backend,
        backends.melonds_backend);
    print_input_seats(target.launch_plan.seats);

    auto network_receiver = std::optional<NetworkInputReceiver>{};
    if (config.input_port.has_value()) {
        network_receiver.emplace(*config.input_port, input_router);
    }

    auto media_server = start_host_media_server_if_needed(HostMediaStartRequest{
        config,
        devices.capture_display,
        devices.display_backend,
        launch_env.gpu.nvenc_cuda_device_id,
        media.config,
        media.destinations,
        media.streams,
    });
    // Plug after Xvfb/Xephyr is up. Soft-fail so a keyboard issue never kills the session.
    if (!plug_virtual_keyboard_with_retry(
            keyboard, devices.use_virtual_capture, launch_env.capture.gamescope_capture)) {
        if (devices.use_virtual_capture && !launch_env.capture.gamescope_capture) {
            std::cerr << "Warning: continuing without remoted keyboard (pads still work).\n";
        }
    }

    auto local_bridge = std::optional<LocalControllerBridge>{};
    if (bridge_device.has_value()) {
        local_bridge.emplace(*bridge_device);
    }

    auto session_runtime = make_session_runtime(target.launch_plan);
    session_runtime->bind_launch_config(std::move(target.launch_config));
    std::cout << session_runtime->info();

    log_direct_emulator_command(session_runtime->launch_config(), target.resolved_retroarch);
    start_emulator_and_verify(*session_runtime, EmulatorStartFailDetail::DirectCli);
    post_emulator_start_warmup(
        media_server.get(),
        config,
        media.streams,
        *session_runtime,
        &streaming_audio,
        std::nullopt,
        gamepads,
        target.launch_plan.players,
        &keyboard,
        launch_env.capture.gamescope_capture,
        launch_env.request.xtest_display);

    auto cadence_tracker = begin_direct_cadence_session(
        target.launch_plan,
        config,
        *session_runtime);

    if (devices.arm_soft_keyboard && devices.standalone_soft_keyboard) {
        std::string display = launch_env.request.xtest_display;
        if (keyboard.plugged()) {
            display = keyboard.capture_display();
        }
        schedule_soft_keyboard(
            devices.standalone_soft_keyboard,
            devices.soft_keyboard_fallback,
            // Prefer OCR of Ryujinx HeaderText when the dialog appears.
            {},
            display,
            session_runtime->emulator().process_id().value_or(0));
    }

    // Start UDP input after the optional A-pulse so it does not race uinput updates.
    if (network_receiver.has_value()) {
        network_receiver->start();
    }

    SessionLoopCadence loop_cadence(
        local_bridge.has_value() ? &*local_bridge : nullptr,
        &input_router,
        &streaming_audio,
        std::nullopt,
        config.audio);
    while (!should_stop() && session_runtime->emulator_running()) {
        loop_cadence.tick();
    }

    if (!should_stop() && !session_end_reason.has_value() && !session_runtime->emulator_running()) {
        const auto code = session_runtime->last_exit_code().value_or(-1);
        const auto stderr_tail = session_runtime->last_stderr_tail();
        std::ostringstream reason;
        reason << format_emulator_exit_summary(code);
        if (session_runtime->launch_config().standalone && launch_env.capture.gamescope_capture) {
            reason << " — if Host GPU is the non-boot NVIDIA, check Gamescope WSI "
                      "(ENABLE_GAMESCOPE_WSI / VK_ADD_IMPLICIT_LAYER_PATH); "
                      "Switch emulators often log \"Device lacks a present queue\"";
        }
        session_end_reason = reason.str();
        std::cerr << "Stopping session: " << *session_end_reason << '\n';
        if (!stderr_tail.empty()) {
            std::cerr << "emulator/gamescope stdio tail:\n" << stderr_tail << '\n';
        } else {
            std::cerr << "(no emulator/gamescope stdio captured)\n";
        }
    }

    if (network_receiver.has_value()) {
        network_receiver->stop();
    }

    // Close XTest before stopping gamescope/Xvfb so Xlib does not abort the process.
    unplug_session_keyboard(&keyboard);
    unregister_session_xtest_display(launch_env.request.session_id);

    untrack_session_audio(&streaming_audio);
    stop_session_runtime(session_runtime);
    if (session_runtime != nullptr) {
        if (const auto code = session_runtime->last_exit_code(); code.has_value()) {
            std::cout << format_emulator_exit_summary(*code) << '\n';
            if (*code == 127) {
                std::cerr
                    << "hint: exit 127 usually means the RetroArch launcher was not found. "
                    << "On Bazzite install: flatpak install flathub org.libretro.RetroArch\n";
            }
        }
    }
    if (backends.switch_backend) {
        sync_and_log_post_exit_switch_saves(
            target.save_profile,
            std::nullopt,
            backends.switch_backend.get(),
            backends.switch_launch_content_stem,
            backends.switch_launch_title_id);
    }
    if (backends.melonds_backend) {
        (void)backends.melonds_backend->post_exit_sync(target.save_profile);
    }
    const std::string end_reason = should_stop()
        ? "host stopped"
        : session_end_reason.value_or("session ended");

    end_direct_cadence_session(cadence_tracker, target.launch_plan, end_reason);
    stop_session_media(media_server);
    if (config.audio) {
        streaming_audio.restore_default_sink();
    }
    cleanup_x11_capture_runtime_dir();

    return 0;
}

int HostApp::run(const std::function<bool()>& should_stop) {
    try {
        std::cout << std::unitbuf;
        std::cerr << std::unitbuf;

        // Last resort only: SessionRuntime RAII owns kill for live sessions.
        // This reaps leftovers from a prior host that never got to run destructors.
        reap_orphaned_emulator_processes();

        {
            auto cadence = archstreamer::cadence::make_runtime_store();
            if (cadence->ensure_ready()) {
                archstreamer::cadence::RuntimeEvent started;
                started.kind = "host_started";
                started.host_id = cadence_host_id();
                started.detail = "host_runner";
                (void)cadence->record_event(started);

                const auto reaped = archstreamer::cadence::reap_stale_instance_state(
                    *cadence,
                    started.host_id);
                if (reaped.sessions_ended > 0 || reaped.claims_released > 0 ||
                    reaped.connections_ended > 0) {
                    std::cout
                        << "cadence: reaped stale state ("
                        << reaped.sessions_ended << " session(s), "
                        << reaped.claims_released << " claim(s), "
                        << reaped.connections_ended << " connection(s))\n";
                }

                const auto save_root = config_.save_root.empty()
                    ? default_save_profile_root()
                    : config_.save_root;
                const auto host_name = !config_.host_name.empty()
                    ? config_.host_name
                    : (config_.username.empty() ? default_cli_username() : config_.username);
                if (!archstreamer::cadence::canonical_identity_name(host_name).empty()) {
                    archstreamer::cadence::HostRecord host;
                    host.identity_id = archstreamer::cadence::identity_id_from_name(host_name);
                    host.host_name = host_name;
                    host.display_name = host_name;
                    host.save_root = save_root.lexically_normal().string();
                    (void)cadence->upsert_host(host);
                }
                const auto imported =
                    archstreamer::cadence::import_users_from_save_root(*cadence, save_root);
                if (imported > 0) {
                    std::cout
                        << "cadence: imported " << imported
                        << " user(s) from save profiles\n";
                }
                const auto backfilled =
                    archstreamer::cadence::backfill_user_profile_paths(*cadence, save_root);
                if (backfilled > 0) {
                    std::cout
                        << "cadence: backfilled profile paths for "
                        << backfilled << " user(s)\n";
                }
            }
        }

        const auto host_id_for_shutdown = cadence_host_id();
        auto release_this_host = [&](std::string_view reason) {
            try {
                auto cadence = archstreamer::cadence::make_runtime_store();
                if (!cadence->ensure_ready()) {
                    return;
                }
                const auto released = archstreamer::cadence::release_host_instance_state(
                    *cadence,
                    host_id_for_shutdown,
                    reason);
                if (released.sessions_ended > 0 || released.claims_released > 0 ||
                    released.connections_ended > 0) {
                    std::cout
                        << "cadence: host shutdown released "
                        << released.sessions_ended << " session(s), "
                        << released.claims_released << " claim(s), "
                        << released.connections_ended << " connection(s)\n";
                }
            } catch (...) {
            }
        };

        auto config = config_;
        std::vector<CatalogScanIssue> scan_issues;
        auto catalog = scan_game_catalog(
            config.rom_root,
            LibretroCoreRegistry::ubuntu_defaults(),
            config.meta_root,
            &scan_issues);
        for (const auto& issue : scan_issues) {
            // Already printed to stderr inside scan; summarize count for operators.
            (void)issue;
        }
        if (!scan_issues.empty()) {
            std::cerr
                << "host: " << scan_issues.size()
                << " title(s) locked (missing Meta, ROM stem mismatch, or invalid .m3m); "
                   "not hosted until fixed.\n";
        }
        const auto list = catalog.list();
        rebuild_catalog_offerings_from_list(list);

        const bool host_plays_locally =
            config.host_role == ParticipantRole::Player &&
            config.bridge_controller_index.has_value();
        apply_host_player_media_policy(config, host_plays_locally);

        StreamingAudioSink streaming_audio;
        prepare_streaming_audio_source(config, streaming_audio);

        if (list.games.empty()) {
            std::cerr << "No supported games found under " << config.rom_root << '\n';
            release_this_host("host exit");
            return 1;
        }

        if (config.list || (!config.selector.has_value() && !config.control_port.has_value())) {
            std::cout << "Found " << list.games.size() << " supported games under " << config.rom_root << ".\n";
            print_game_catalog(std::cout, list);
            release_this_host("host exit");
            return config.list ? 0 : 2;
        }

        auto bridge_device = resolve_bridge_device(config);
        auto bridge_identity = std::optional<HostPlayerControllerIdentity>{};
        if (bridge_device.has_value()) {
            bridge_identity = host_player_controller_identity(*bridge_device);
        }

        int result = 0;
        if (config.control_port.has_value()) {
            if (should_stop()) {
                release_this_host("host stopped");
                return 0;
            }
            result = run_lobby_sessions(config, catalog, list, streaming_audio, bridge_device, should_stop);
        } else {
            result = run_direct_session(
                config,
                catalog,
                list,
                streaming_audio,
                bridge_device,
                bridge_identity,
                host_plays_locally,
                should_stop);
        }
        release_this_host(should_stop() ? "host stopped" : "host exit");
        return result;
    } catch (const std::exception& error) {
        StreamingAudioSink{}.restore_default_sink();
        cleanup_x11_capture_runtime_dir();
        try {
            auto cadence = archstreamer::cadence::make_runtime_store();
            if (cadence->ensure_ready()) {
                (void)archstreamer::cadence::release_host_instance_state(
                    *cadence,
                    cadence_host_id(),
                    should_stop() ? "host stopped" : "host error");
            }
        } catch (...) {
        }
        if (should_stop()) {
            std::cout << "Host stopped.\n";
            return 0;
        }
        std::cerr << "host_runner: " << error.what() << '\n';
        return 1;
    }
}

} // namespace archstreamer
