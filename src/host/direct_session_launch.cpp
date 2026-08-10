#include "host/direct_session_launch.hpp"

#include "common/catalog_presenter.hpp"
#include "common/cli_common.hpp"
#include "common/ds_touch_mapping.hpp"
#include "common/participant_role.hpp"
#include "common/steam_art_import.hpp"
#include "client/controller_backend.hpp"
#include "host/cadence_session_events.hpp"
#include "host/gpu_select.hpp"
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

SessionGpuSelection resolve_direct_gpu_selection(
    const HostAppConfig& config,
    bool use_virtual_capture,
    bool virtualgl_capture,
    bool gamescope_capture) {
    SessionGpuSelection selection;
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

std::optional<SessionLaunchTarget> prepare_direct_session_target(
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

    return SessionLaunchTarget{
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


SessionGpuSelection prepare_direct_gpu(
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

SessionMediaPlan build_direct_media_plan(const HostAppConfig& config) {
    SessionMediaPlan plan;
    plan.config = media_plan_config_for(config);
    if (config.video || config.audio) {
        plan.destinations = media_destinations_for_host(plan.config);
        plan.streams = media_streams_for_dry_run(plan.config, plan.destinations);
    }
    return plan;
}

SessionDevicePlan resolve_direct_device_plan(
    const HostLaunchPlan& launch_plan,
    const HostAppConfig& config,
    const CapturePlan& capture) {
    auto pads = resolve_direct_pad_selection(config, launch_plan);
    SessionDevicePlan devices;
    devices.shared_pad_plan = std::move(pads.shared_pad_plan);
    devices.resolved_indices = std::move(pads.resolved_indices);
    devices.resolved_pads = std::move(pads.resolved_pads);
    devices.virtual_joypad_index = pads.virtual_joypad_index;
    apply_capture_to_session_device_plan(devices, config, capture);
    return devices;
}

void prepare_direct_backend(
    const HostAppConfig& config,
    RetroArchLaunchConfig& launch_config,
    const CapturePlan& capture,
    const SessionLaunchTarget& target,
    HostLaunchPlan& launch_plan,
    SessionDevicePlan& devices,
    VirtualKeyboard& keyboard,
    EmulatorLaunchEnvRequest& launch_env_request,
    SessionBackendState& backends) {
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

} // namespace archstreamer
