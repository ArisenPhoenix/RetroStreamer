#include "common/catalog_presenter.hpp"
#include "common/cli_common.hpp"
#include "common/participant_role.hpp"
#include "common/platform/default_platform.hpp"
#include "common/platform/process_utils.hpp"
#include "common/steam_art_import.hpp"
#include "client/controller_backend.hpp"
#include "host/capture_platform.hpp"
#include "host/emulator_orphan_reaper.hpp"
#include "host/game_catalog.hpp"
#include "host/game_catalog_scanner.hpp"
#include "host/gpu_select.hpp"
#include "host/host_app.hpp"
#include "host/host_app_config.hpp"
#include "host/host_concurrent_lobby.hpp"
#include "host/streaming_audio_sink.hpp"
#include "host/host_launch_planner.hpp"
#include "host/host_session_helpers.hpp"
#include "host/input_router.hpp"
#include "host/launch_environment.hpp"
#include "host/virtual_keyboard.hpp"
#include "host/soft_keyboard_host.hpp"
#include "host/local_controller_bridge.hpp"
#include "host/network_input_receiver.hpp"
#include "host/platform/default_host_platform.hpp"
#include "host/retroarch_config_writer.hpp"
#include "host/retroarch_resolve.hpp"
#include "host/save_profile.hpp"
#include "host/session_launch_assemble.hpp"
#include "host/standalone_emulator.hpp"
#include "host/session_lobby.hpp"
#include "host/session_runtime.hpp"
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
    // Soft-keyboard bridge for the no-session launch path. The watcher holds a weak
    // reference, so this has to outlive the launch block or it retires immediately.
    std::shared_ptr<SoftKeyboardHostBridge> standalone_soft_keyboard;
    std::optional<std::string> session_end_reason;

    // Direct (non-lobby) launch path — single local session, no control port.
    const auto game_id = select_game_for_launch(list, *config.selector);
    if (!game_id.has_value()) {
        std::cerr << "Game not found: " << *config.selector << '\n';
        return 1;
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

    const auto save_profile = prepare_save_profile(config.save_root, launch_plan.save_username);

    auto launch_config = catalog.launch_config_for(launch_plan.game_id);
    const auto resolved_retroarch = resolve_retroarch();
    std::string system_key;
    if (const auto hosted = catalog.find_hosted(launch_plan.game_id); hosted.has_value()) {
        system_key = hosted->get().info.system_key;
    } else if (const auto info = catalog.find(launch_plan.game_id); info.has_value()) {
        system_key = info->system_key;
    }
    if (!launch_config.standalone && system_key == "ps2") {
        std::cout
            << "PS2 memcards: " << user_ps2_memcard_directory(save_profile) << '\n';
    }
    // LRPS2/PCSX ReARMed stall badly under RetroArch's sdl2 joypad poll. Keep udev
    // for PlayStation even if a caller/GUI asked for sdl2.
    if (!launch_config.standalone &&
        (system_key == "ps1" || system_key == "ps2" || system_key == "psp") &&
        config.retroarch_joypad_driver == "sdl2") {
        std::cout
            << "Note: forcing joypad driver udev for " << system_key
            << " (sdl2 stalls PlayStation cores).\n";
        config.retroarch_joypad_driver = "udev";
    }
    if (!launch_config.standalone) {
        launch_config.retroarch_path = resolved_retroarch.display_path;
        launch_config.command_prefix = resolved_retroarch.argv_prefix;
    }
    // Avoid -f on the host's real Wayland session (can exit immediately). When video
    // streams from a virtual display, force fullscreen via the override config instead.
    if (config.verbose && !launch_config.standalone) {
        launch_config.extra_args.insert(launch_config.extra_args.begin(), "--verbose");
    }
    // Streaming already forced off above for Host Player.
    // Emulator child env is assembled once later (audio/input/gpu/capture/emulator).
    if (bridge_device.has_value() && !config.ignore_controller.has_value()) {
        if (bridge_device->vendor_id != 0 && bridge_device->product_id != 0) {
            config.ignore_controller = hex_vid_pid(bridge_device->vendor_id, bridge_device->product_id);
        }
    }
    // Blacklist every physical pad currently attached so RetroArch is less likely to
    // bind P1 to a host controller. Virtual ArchStreamer pads are created after this.
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
            std::cerr << "Warning: host controller scan for ignore list failed: " << error.what() << '\n';
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
    if (config.retroarch_joypad_driver != "sdl2") {
        std::cerr
            << "Warning: SDL_GAMECONTROLLER_IGNORE_DEVICES only affects RetroArch when "
            << "--retroarch-joypad-driver is sdl2.\n";
    }

    // Host Player keeps the real DISPLAY (and speakers). Streamed RetroArch needs a
    // virtual capture surface. Switch standalone defaults to headless gamescope on Linux;
    // Windows captures the desktop/HWND via d3d11screencapturesrc (no gamescope).
    const auto capture = resolve_capture_plan(config, launch_config);
    const bool use_virtual_capture = capture.use_virtual_capture;
    const bool capture_fullscreen = capture.capture_fullscreen;
    const std::string capture_display = capture.capture_display;
    const auto display_backend = capture.display_backend;
    const bool gamescope_capture = capture.gamescope_capture;
    const bool virtualgl_capture = capture.virtualgl_capture;

    EmulatorLaunchEnvRequest launch_env_request;
    launch_env_request.stream_media = config.audio || config.video;
    launch_env_request.stream_audio = config.audio;
    launch_env_request.host_plays_locally = host_plays_locally;
    launch_env_request.audio_source = config.audio_source;
    launch_env_request.ignore_devices = *config.ignore_controller;
    launch_env_request.use_virtual_capture = use_virtual_capture;
    launch_env_request.gamescope_capture = gamescope_capture;
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
        const bool same_as_encode =
            resolved_encode.has_value() && resolved_encode->id == resolved_gpu->id;
        if (same_as_encode) {
            std::cout
                << "GPU: " << resolved_gpu->name
                << " [" << resolved_gpu->id << "] (encode+render)";
        } else {
            if (resolved_encode.has_value()) {
                std::cout
                    << "Encode GPU: " << resolved_encode->name
                    << " [" << resolved_encode->id << "]";
                if (resolved_encode->nvidia_index >= 0) {
                    std::cout << " nvidia_index=" << resolved_encode->nvidia_index;
                }
                std::cout << '\n';
            }
            std::cout
                << "Render GPU: " << resolved_gpu->name
                << " [" << resolved_gpu->id << "]";
            if (config.separate_render_gpu) {
                std::cout << " (separate from encode)";
            }
        }
        if (resolved_gpu->vulkan_index >= 0) {
            std::cout << " vulkan_index=" << resolved_gpu->vulkan_index;
        }
        if (!resolved_gpu->prime_provider.empty()) {
            std::cout << " prime=" << resolved_gpu->prime_provider;
        }
        std::cout << '\n';
        if (const auto vd = pci_vendor_device_id(resolved_gpu->pci_bus); vd.has_value()) {
            gamescope_vk_device = *vd;
        }
        // PRIME offload for NVIDIA. On plain Xvfb the provider name is ignored (no RandR
        // providers); VirtualGL uses the real display's GLX where G0/G1 selection works.
        launch_env_request.render_gpu = *resolved_gpu;
        if (resolved_gpu->prime_provider.empty() && resolved_gpu->nvidia_index >= 0) {
            std::cerr
                << "Warning: NVIDIA GPU selected but no PRIME provider was mapped "
                << "(is DISPLAY set when scanning GPUs?). Capture GL may use llvmpipe.\n";
        }
        if (use_virtual_capture && !virtualgl_capture && !gamescope_capture &&
            resolved_gpu->nvidia_index > 0) {
            std::cerr
                << "Warning: streamed OpenGL on plain Xvfb cannot select NVIDIA GPU index "
                << resolved_gpu->nvidia_index
                << " (always uses nvidia:0). Install VirtualGL (vglrun) so Host GPU works.\n";
        }
    } else if (resolved_encode.has_value()) {
        std::cout
            << "Encode GPU: " << resolved_encode->name
            << " [" << resolved_encode->id << "]";
        if (resolved_encode->nvidia_index >= 0) {
            std::cout << " nvidia_index=" << resolved_encode->nvidia_index;
        }
        std::cout << '\n';
    }
    if (gamescope_vk_device.empty()) {
        // Prefer RTX 3060 then 1660 Ti on this host when auto-detect fails.
        gamescope_vk_device = "10de:2504";
    }

    const auto media_config = media_plan_config_for(config);
    auto media_destinations = std::vector<HostMediaDestination>{};
    auto media_streams = std::vector<MediaClientStream>{};
    if (config.video || config.audio) {
        media_destinations = media_destinations_for_host(media_config);
        media_streams = media_streams_for_dry_run(media_config, media_destinations);
    }

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

    if (config.dry_run) {
        return 0;
    }

    HostVirtualGamepadBus gamepads(launch_plan.virtual_identities);
    for (RetroArchPort port = 0; port < launch_plan.players; ++port) {
        gamepads.plug(port);
    }
    // Virtual keyboard needs the capture display (:99) which media_server starts later.
    VirtualKeyboard keyboard(capture_display);
    std::this_thread::sleep_for(std::chrono::milliseconds(750));

    // Resolve joypad indices after uinput pads appear. Prefer discovered index so
    // RetroArch binds the ArchStreamer pad even when host controllers remain visible.
    // udev and sdl2 enumerate pads differently — match the driver RetroArch will use.
    std::vector<std::size_t> resolved_indices;
    std::vector<ArchStreamerSdlPad> resolved_pads;
    if (config.retroarch_joypad_driver == "udev") {
        if (config.verbose) {
            std::cout << "udev joysticks (ArchStreamer hunt):\n";
        }
        resolved_indices = find_archstreamer_udev_joypad_indices(
            launch_plan.players,
            config.verbose);
    } else {
        resolved_pads = find_archstreamer_sdl_pads(
            launch_plan.players,
            config.ignore_controller.value_or(""),
            config.verbose);
        resolved_indices.reserve(resolved_pads.size());
        for (const auto& pad : resolved_pads) {
            resolved_indices.push_back(pad.sdl_index);
        }
    }
    std::size_t virtual_joypad_index = 0;
    if (config.virtual_joypad_index.has_value()) {
        virtual_joypad_index = *config.virtual_joypad_index;
        std::cout << "Using explicit --virtual-joypad-index " << virtual_joypad_index << '\n';
    } else if (!resolved_indices.empty()) {
        virtual_joypad_index = resolved_indices.front();
        if (config.verbose) {
            std::cout
                << "Resolved virtual joypad index " << virtual_joypad_index
                << " (driver=" << config.retroarch_joypad_driver << ")\n";
        }
    } else {
        std::cerr
            << "Warning: ArchStreamer virtual pads not visible to "
            << config.retroarch_joypad_driver
            << " yet; defaulting RetroArch joypad index to 0.\n";
    }

    if (launch_config.standalone || system_key == "switch") {
        // Re-verify at launch so a stale catalog / missing install cannot start.
        const auto runtime = resolve_switch_runtime();
        if (!runtime.has_value()) {
            throw std::runtime_error(switch_runtime_unavailable_message());
        }
        launch_config.standalone = true;
        launch_config.core_path = runtime->path;
        launch_config.standalone_args_before_content = runtime->args_before_content;
    }

    if (launch_config.standalone) {
        const auto profile_name =
            preferred_steam_or_username_display_name(save_profile.username);
        auto switch_prep = prepare_switch_standalone(
            launch_config,
            SwitchStandalonePrepInput{
                save_profile,
                launch_plan.players,
                config.verbose,
                /*product_id_base=*/0,
                config.ignore_controller.value_or(""),
                config.graphics_api,
                virtualgl_capture,
                gamescope_capture,
                config.resolution.switch_scale,
                &resolved_gpu,
                profile_name,
                std::move(resolved_pads),
            });
        resolved_pads = std::move(switch_prep.resolved_pads);
        if (switch_prep.use_ryujinx) {
            launch_env_request.ryujinx_profile = std::move(switch_prep.ryujinx_profile);
            const auto& ryujinx_user = *launch_env_request.ryujinx_profile;
            std::cout
                << "Ryujinx (ldn_mitm) config: " << ryujinx_user.data_root
                << "\nRyujinx keys:            " << ryujinx_user.keys_directory
                << "\nShared Switch saves:     " << switch_prep.synced_title_count
                << " title(s)\n";
            {
                const int scale = std::clamp(config.resolution.switch_scale, 1, 4);
                std::cout << "Ryujinx resolution: " << scale << "x native\n";
            }
        } else {
            launch_env_request.yuzu_profile = std::move(switch_prep.yuzu_profile);
            const auto& yuzu_user = *launch_env_request.yuzu_profile;
            std::cout
                << "Yuzu renderer: "
                << (switch_prep.force_opengl
                        ? "OpenGL"
                        : switch_prep.force_vulkan ? "Vulkan" : "default");
            if (switch_prep.yuzu_vulkan_device >= 0 && resolved_gpu.has_value()) {
                std::cout
                    << " (vulkan_device=" << switch_prep.yuzu_vulkan_device
                    << " → " << resolved_gpu->name << ")";
            }
            std::cout << '\n';
            {
                const int scale = std::clamp(config.resolution.switch_scale, 1, 6);
                std::cout << "Switch resolution: " << scale << "x native"
                          << " (resolution_setup=" << (scale + 1) << ")\n";
            }
            std::cout
                << "Yuzu user data: " << yuzu_user.xdg_data_home
                << "\nYuzu keys:      " << yuzu_user.keys_directory << '\n';
        }
        if (switch_prep.enable_soft_keyboard) {
            ensure_soft_keyboard(
                standalone_soft_keyboard,
                profile_name,
                "What is your name?",
                capture_display);
        }

    } else {
        RetroArchOverrideParams override_params;
        override_params.first_virtual_joypad_index = virtual_joypad_index;
        override_params.identities = &launch_plan.virtual_identities;
        override_params.joypad_driver = config.retroarch_joypad_driver;
        override_params.players = launch_plan.players;
        override_params.save_profile = &save_profile;
        override_params.realtime_pacing = config.audio || config.video;
        override_params.capture_fullscreen = capture_fullscreen && use_virtual_capture;
        override_params.capture_resolution = config.video_resolution;
        override_params.vulkan_gpu_index =
            (!use_virtual_capture && resolved_gpu.has_value()) ? resolved_gpu->vulkan_index : -1;
        override_params.system_key = system_key;
        override_params.core_path = launch_config.core_path;
        override_params.resolution_scale = config.resolution.retroarch_scale;
        const auto runtime_override = apply_retroarch_override(launch_config, override_params);
        std::cout
            << "RetroArch config: " << runtime_override
            << "\nVirtual joypad index: " << virtual_joypad_index
            << " (driver=" << config.retroarch_joypad_driver << ")\n";
        {
            const int scale = std::clamp(config.resolution.retroarch_scale, 1, 6);
            std::cout << "RetroArch resolution: " << scale << "x native"
                      << " (known cores via .opt)\n";
        }
        if (!system_key.empty()) {
            std::cout << "Face buttons: system=" << system_key
                      << " (" << face_button_map_name(system_key) << ")\n";
        }
    }

    apply_capture_and_launch_environment(
        launch_config,
        capture,
        config,
        gamescope_vk_device,
        resolved_gpu,
        launch_env_request);

    if (capture_fullscreen) {
        std::cout
            << "Capture fullscreen: " << config.video_resolution
            << " on display " << capture_display
            << (use_virtual_capture ? " (virtual)" : " (host)") << '\n';
    }

    // Pin Viewer RetroArch to the capture null sink (speakers stay quiet unless Watch-local).
    if (config.audio) {
        streaming_audio.park_game_audio();
    }

    InputRouter input_router(gamepads, &keyboard);
    input_router.set_seat_assignment(launch_plan.seats);
    std::cout << "Input seats: " << launch_plan.seats.seats.size() << '\n';
    for (const auto& seat : launch_plan.seats.seats) {
        std::cout
            << "  client " << static_cast<int>(seat.client_id)
            << " local P" << static_cast<int>(seat.local_player) + 1
            << " -> RetroArch P" << static_cast<int>(seat.retroarch_port) + 1 << '\n';
    }

    auto network_receiver = std::optional<NetworkInputReceiver>{};
    if (config.input_port.has_value()) {
        network_receiver.emplace(*config.input_port, input_router);
    }

    auto media_server = std::unique_ptr<MediaServer>{};
    if (config.audio || config.video) {
        normalize_audio_backend_for_platform(config);
        media_server = make_host_media_server(GStreamerMediaCaptureConfig{
            config.video,
            config.audio,
            capture_display,
            config.video_resolution,
            display_backend,
            config.audio_backend,
            config.audio_source,
            config.verbose,
            nvenc_cuda_device_id,
        });
        media_server->start(media_config, media_destinations, media_streams);
    }
    // Plug after Xvfb/Xephyr is up. Soft-fail so a keyboard issue never kills the session.
    if (use_virtual_capture && !gamescope_capture) {
        bool keyboard_ready = false;
        for (int attempt = 0; attempt < 20; ++attempt) {
            try {
                keyboard.plug();
                keyboard_ready = true;
                break;
            } catch (const std::exception& error) {
                if (attempt == 19) {
                    std::cerr << "Warning: virtual keyboard unavailable: " << error.what() << '\n';
                } else {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
            }
        }
        if (!keyboard_ready) {
            std::cerr << "Warning: continuing without remoted keyboard (pads still work).\n";
        }
    } else if (!use_virtual_capture) {
        try {
            keyboard.plug();
        } catch (const std::exception& error) {
            std::cerr << "Warning: virtual keyboard unavailable: " << error.what() << '\n';
        }
    }

    auto local_bridge = std::optional<LocalControllerBridge>{};
    if (bridge_device.has_value()) {
        local_bridge.emplace(*bridge_device);
    }

    auto session_runtime = make_session_runtime(launch_plan);
    session_runtime->bind_launch_config(std::move(launch_config));
    std::cout
        << "Session runtime: " << session_runtime->kind_name()
        << " (shared_emulator=" << (session_runtime->uses_shared_emulator() ? "yes" : "no")
        << ", instances=" << static_cast<int>(session_runtime->emulator_instance_count())
        << ", logical_host_client=" << static_cast<int>(session_runtime->logical_host_client_id())
        << ", save_user=" << session_runtime->save_username()
        << ")\n";

    {
        const auto& launch_config = session_runtime->launch_config();
        std::string command;
        if (launch_config.standalone) {
            command.clear();
            for (const auto& arg : launch_config.command_prefix) {
                if (!command.empty()) {
                    command.push_back(' ');
                }
                command += arg;
            }
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
            std::cout << "Launching standalone emulator...\nCommand: " << command << '\n';
        } else {
            command.clear();
            for (const auto& arg : launch_config.command_prefix) {
                if (!command.empty()) {
                    command.push_back(' ');
                }
                command += arg;
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
            std::cout << "Launching RetroArch...\nCommand: " << command << '\n';
        }
    }
    session_runtime->start_emulator();
    // Flatpak RetroArch can take a moment; failed exec exits almost immediately.
    for (int i = 0; i < 10 && session_runtime->emulator_running(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (!session_runtime->emulator_running()) {
        const auto code = session_runtime->last_exit_code().value_or(127);
        const auto stderr_tail = session_runtime->last_stderr_tail();
        if (session_runtime->launch_config().standalone) {
            std::string message =
                "Standalone emulator exited immediately (code " + std::to_string(code) + "). "
                "Check Ryujinx/Yuzu install and keys under ~/.local/share/archstreamer/ "
                "(ryujinx/ or yuzu/) and per-user data under the save profile Switch dirs.";
            if (!stderr_tail.empty()) {
                message += "\n\n" + stderr_tail;
            }
            throw std::runtime_error(message);
        }
        std::string message =
            "RetroArch exited immediately (code " + std::to_string(code) + "). "
            "Common causes: missing BIOS/firmware under ~/.config/retroarch/system "
            "(PS2 needs files in system/pcsx2/bios), a broken core, or RetroArch not runnable.";
        if (!stderr_tail.empty()) {
            message += "\n\n" + stderr_tail;
        }
        throw std::runtime_error(message);
    }
    start_deferred_gamescope_video_if_needed(
        media_server.get(),
        config,
        media_streams,
        session_runtime->emulator().process_id().value_or(0));
    if (config.audio) {
        // Pulse connects asynchronously; re-park after the sink-input appears.
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        streaming_audio.park_game_audio();
    }

    if (config.pulse_input && launch_plan.players > 0) {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        pulse_virtual_pad_a(gamepads);
    }

    // Start UDP input after the optional A-pulse so it does not race uinput updates.
    if (network_receiver.has_value()) {
        network_receiver->start();
    }

    auto next_audio_park = std::chrono::steady_clock::now();
    while (!should_stop() && session_runtime->emulator_running()) {
        if (local_bridge.has_value()) {
            local_bridge->update(input_router);
        }
        if (config.audio) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= next_audio_park) {
                streaming_audio.park_game_audio();
                next_audio_park = now + std::chrono::seconds(3);
            }
        }

        // Network pads run on their own thread. Local bridge still needs a short cadence.
        if (local_bridge.has_value()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    if (!should_stop() && !session_end_reason.has_value() && !session_runtime->emulator_running()) {
        const auto code = session_runtime->last_exit_code().value_or(-1);
        const auto stderr_tail = session_runtime->last_stderr_tail();
        std::ostringstream reason;
        reason << "emulator exited (code " << code << ")";
        if (session_runtime->launch_config().standalone && gamescope_capture) {
            reason << " — if Host GPU is the non-boot NVIDIA, check Gamescope WSI "
                      "(ENABLE_GAMESCOPE_WSI / VK_ADD_IMPLICIT_LAYER_PATH); "
                      "Switch emulators often log \"Device lacks a present queue\"";
        }
        session_end_reason = reason.str();
        std::cerr << "Stopping session: " << *session_end_reason << '\n';
        if (!stderr_tail.empty()) {
            std::cerr << stderr_tail << '\n';
        }
    }

    if (network_receiver.has_value()) {
        network_receiver->stop();
    }

    session_runtime->stop_emulator();
    if (const auto code = session_runtime->last_exit_code(); code.has_value()) {
        std::cout << "RetroArch exited with code " << *code << '\n';
        if (*code == 127) {
            std::cerr
                << "hint: exit 127 usually means the RetroArch launcher was not found. "
                << "On Bazzite install: flatpak install flathub org.libretro.RetroArch\n";
        }
    }
    sync_and_log_post_exit_switch_saves(save_profile);
    // Close XTest before Xvfb so Xlib does not abort the process.
    keyboard.unplug();
    if (media_server) {
        media_server->stop();
        media_server.reset();
    }
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

        auto config = config_;
        auto catalog = scan_game_catalog(config.rom_root, LibretroCoreRegistry::ubuntu_defaults(), config.meta_root);
        const auto list = catalog.list();

        const bool host_plays_locally =
            config.host_role == ParticipantRole::Player &&
            config.bridge_controller_index.has_value();
        apply_host_player_media_policy(config, host_plays_locally);

        StreamingAudioSink streaming_audio;
        prepare_streaming_audio_source(config, streaming_audio);

        if (list.games.empty()) {
            std::cerr << "No supported games found under " << config.rom_root << '\n';
            return 1;
        }

        if (config.list || (!config.selector.has_value() && !config.control_port.has_value())) {
            std::cout << "Found " << list.games.size() << " supported games under " << config.rom_root << ".\n";
            print_game_catalog(std::cout, list);
            return config.list ? 0 : 2;
        }

        auto bridge_device = resolve_bridge_device(config);
        auto bridge_identity = std::optional<HostPlayerControllerIdentity>{};
        if (bridge_device.has_value()) {
            bridge_identity = host_player_controller_identity(*bridge_device);
        }

        if (config.control_port.has_value()) {
            if (should_stop()) {
                return 0;
            }
            return run_lobby_sessions(config, catalog, list, streaming_audio, bridge_device, should_stop);
        }

        return run_direct_session(
            config,
            catalog,
            list,
            streaming_audio,
            bridge_device,
            bridge_identity,
            host_plays_locally,
            should_stop);
    } catch (const std::exception& error) {
        StreamingAudioSink{}.restore_default_sink();
        cleanup_x11_capture_runtime_dir();
        if (should_stop()) {
            std::cout << "Host stopped.\n";
            return 0;
        }
        std::cerr << "host_runner: " << error.what() << '\n';
        return 1;
    }
}

} // namespace archstreamer
