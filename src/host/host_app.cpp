#include "common/catalog_presenter.hpp"
#include "common/cli_common.hpp"
#include "common/participant_role.hpp"
#include "common/platform/default_platform.hpp"
#include "common/platform/process_utils.hpp"
#include "client/controller_backend.hpp"
#include "host/game_catalog_scanner.hpp"
#include "host/gpu_select.hpp"
#ifndef _WIN32
#include "host/gstreamer_media_server.hpp"
#include "host/virtual_display.hpp"
#endif
#include "host/host_app.hpp"
#include "host/host_app_config.hpp"
#include "host/streaming_audio_sink.hpp"
#include "host/host_launch_planner.hpp"
#include "host/host_session_helpers.hpp"
#include "host/input_router.hpp"
#include "host/launch_environment.hpp"
#include "host/virtual_keyboard.hpp"
#include "host/local_controller_bridge.hpp"
#include "host/network_input_receiver.hpp"
#include "host/platform/default_host_platform.hpp"
#include "host/retroarch_config_writer.hpp"
#include "host/retroarch_netcmd.hpp"
#include "host/retroarch_resolve.hpp"
#include "host/save_profile.hpp"
#include "host/standalone_emulator.hpp"
#include "host/session_control_monitor.hpp"
#include "host/session_service.hpp"
#include "host/virtual_joypad_resolve.hpp"
#include "host/link_cable_backend.hpp"

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

int HostApp::run(const std::function<bool()>& should_stop) {
    try {
        std::cout << std::unitbuf;
        std::cerr << std::unitbuf;

        auto config = config_;
        auto catalog = scan_game_catalog(config.rom_root, LibretroCoreRegistry::ubuntu_defaults(), config.meta_root);
        const auto list = catalog.list();

        const bool host_plays_locally =
            config.host_role == ParticipantRole::Player &&
            config.bridge_controller_index.has_value();
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

        StreamingAudioSink streaming_audio;
        if (config.audio) {
            if (config.audio_source.empty()) {
                try {
                    config.audio_source = streaming_audio.monitor_source();
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
        }

        if (list.games.empty()) {
            std::cerr << "No supported games found under " << config.rom_root << '\n';
            return 1;
        }

        if (config.list || (!config.selector.has_value() && !config.control_port.has_value())) {
            std::cout << "Found " << list.games.size() << " supported games under " << config.rom_root << ".\n";
            print_game_catalog(std::cout, list);
            return config.list ? 0 : 2;
        }

        auto bridge_device = std::optional<ControllerDevice>{};
        if (config.host_role == ParticipantRole::Viewer && config.bridge_controller_index.has_value()) {
            throw std::runtime_error("--bridge-controller cannot be used with --host-role viewer");
        }
        if (config.host_role == ParticipantRole::Player && !config.bridge_controller_index.has_value()) {
            throw std::runtime_error(
                "--host-role player requires --bridge-controller "
                "(or use --host-role viewer for a dedicated streaming host)");
        }
        if (config.host_role == ParticipantRole::Player && config.bridge_controller_index.has_value()) {
            bridge_device = local_bridge_device_for(*config.bridge_controller_index);
        }
        auto bridge_identity = std::optional<HostPlayerControllerIdentity>{};
        if (bridge_device.has_value()) {
            bridge_identity = host_player_controller_identity(*bridge_device);
        }

        auto launch_plan = HostLaunchPlan{};
        auto session_plan = std::optional<SessionPlan>{};
        const auto initial_ignore_controller = config.ignore_controller;
        const bool session_lobby_mode = config.control_port.has_value();
        std::optional<std::string> session_end_reason;

        // Session hosts return to the lobby after a game ends so clients can pick a new title
        // without the GUI/host_runner process exiting.
        for (;;) {
        if (session_lobby_mode) {
            if (should_stop()) {
                return 0;
            }
            if (!config.input_port.has_value()) {
                config.input_port = 45454;
            }
            auto host_hello = std::optional<ClientHello>{};
            if (bridge_device.has_value()) {
                if (!config.selector.has_value()) {
                    throw std::runtime_error("--bridge-controller in session mode requires a game selector");
                }
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
                host_hello = host_player_hello_for_session(
                    config.username,
                    *selected_game,
                    config.session_mode,
                    *bridge_identity);
            }
            HostSessionService session_service(
                *config.control_port,
                config.clients,
                list,
                std::chrono::seconds(config.session_timeout_seconds),
                std::move(host_hello),
                should_stop,
                config.art_root.empty()
                    ? (config.rom_root.parent_path() / "Art")
                    : config.art_root);
            session_plan = session_service.wait_for_ready_session();
            if (should_stop()) {
                return 0;
            }
            // Prefer udev for RetroArch: sdl2 joypad polling stalls heavy cores (LRPS2).
            // Virtual uinput pads are visible to udev; we bind by resolved js index below.
            config.ignore_controller = initial_ignore_controller;
            if (!config.ignore_controller.has_value()) {
                config.ignore_controller = sdl_ignore_list_for_session(*session_plan);
            }
            launch_plan = launch_plan_for_session(*session_plan);
        } else {
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
            launch_plan = launch_plan_for_direct(
                *selected_game,
                config.session_mode,
                config.players,
                config.username,
                bridge_identity);
        }

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
            stage_user_ps2_memcards(save_profile);
            std::cout
                << "PS2 memcards: staged from "
                << user_ps2_memcard_directory(save_profile) << '\n';
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
        if (session_plan.has_value()) {
            session_plan->system_key = system_key;
            if (system_key == "gb" || system_key == "gbc" || system_key == "gb-gbc") {
                LinkCableBackend::write_single_gb_core_options();
            }
            if (const auto hosted = catalog.find_hosted(launch_plan.game_id); hosted.has_value()) {
                session_plan->playlist_discs = hosted->get().info.playlist_discs;
                session_plan->current_disc_index = 0;
                session_plan->retroarch_netcmd_port = DefaultRetroArchNetcmdPort;
                if (!session_plan->playlist_discs.empty()) {
                    std::cout
                        << "Multi-disc playlist: " << session_plan->playlist_discs.size()
                        << " disc(s); netcmd port " << session_plan->retroarch_netcmd_port << '\n';
                }
            }
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
        // virtual capture surface. Switch/Yuzu defaults to headless gamescope on Linux;
        // Windows captures the desktop/HWND via d3d11screencapturesrc (no gamescope).
        const bool use_virtual_capture = config.video;
        const bool capture_fullscreen = config.video;
        const std::string capture_display = config.virtual_display;
        auto display_backend = config.display_backend;
#ifndef _WIN32
        if (launch_config.standalone && use_virtual_capture &&
            display_backend == VirtualDisplayBackend::None) {
            display_backend = VirtualDisplayBackend::Gamescope;
        }
        if (!launch_config.standalone && use_virtual_capture &&
            (display_backend == VirtualDisplayBackend::None ||
             display_backend == VirtualDisplayBackend::Xvfb) &&
            core_needs_gl_on_virtual_display(launch_config.core_path) &&
            find_vglrun().has_value() && command_available("Xvfb")) {
            // Only wrap HW GL cores. Software cores (gambatte/…) use plain Xvfb +
            // sdl2 — vglrun left ximagesrc stuck on static GB credits/title frames.
            display_backend = VirtualDisplayBackend::VirtualGL;
        } else if (!launch_config.standalone && use_virtual_capture &&
                   display_backend == VirtualDisplayBackend::VirtualGL &&
                   !core_needs_gl_on_virtual_display(launch_config.core_path)) {
            std::cout
                << "Display: plain Xvfb for software core (skipping VirtualGL)\n";
            display_backend = VirtualDisplayBackend::Xvfb;
        }
        // Software cores (GB, etc.): keep the virtual framebuffer modest so scene-cut
        // IDRs fit in fewer UDP datagrams. 1080p captures of a 160×144 game were
        // freezing Wi‑Fi Flatpak clients (tiny SO_RCVBUF) until title animation.
        if (!launch_config.standalone && use_virtual_capture &&
            !core_needs_gl_on_virtual_display(launch_config.core_path)) {
            int width = 0;
            int height = 0;
            const auto x_pos = config.video_resolution.find('x');
            if (x_pos != std::string::npos) {
                try {
                    width = std::stoi(config.video_resolution.substr(0, x_pos));
                    height = std::stoi(config.video_resolution.substr(x_pos + 1));
                } catch (const std::exception&) {
                }
            }
            if (width > 1280 || height > 720) {
                std::cout
                    << "Capture resolution capped to 1280x720 for software core "
                    << "(was " << config.video_resolution << ")\n";
                config.video_resolution = "1280x720";
            }
        }
#endif
        const bool gamescope_capture =
#ifndef _WIN32
            use_virtual_capture && display_backend == VirtualDisplayBackend::Gamescope;
#else
            false;
#endif
        const bool virtualgl_capture =
#ifndef _WIN32
            use_virtual_capture && display_backend == VirtualDisplayBackend::VirtualGL;
#else
            false;
#endif

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
            if (session_plan.has_value()) {
                media_destinations = media_destinations_for_session(media_config, *session_plan);
                media_streams = media_streams_for_dry_run(media_config, media_destinations);
            } else {
                media_destinations = media_destinations_for_host(media_config);
                media_streams = media_streams_for_dry_run(media_config, media_destinations);
            }
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
            if (session_plan.has_value()) {
                for (const auto& stream : media_streams) {
                    if (stream.client_id == HostClientId) {
                        continue;
                    }
                    send_media_endpoint_to_client(*session_plan, stream.client_id, stream.endpoint);
                }
                send_session_starting_to_clients(*session_plan);
                send_session_ended_to_clients(*session_plan, "dry run complete");
            }
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
            const auto yuzu = ensure_yuzu_runtime();
            if (!yuzu.has_value()) {
                const auto message = yuzu_unavailable_message();
                if (session_plan.has_value()) {
                    send_error_to_session_clients(*session_plan, message);
                }
                throw std::runtime_error(message);
            }
            launch_config.standalone = true;
            launch_config.core_path = yuzu->path;
            launch_config.standalone_args_before_content = yuzu->args_before_content;
        }

        if (launch_config.standalone) {
            // Yuzu bindings need SDL GUIDs even when RetroArch would use udev.
            if (resolved_pads.empty()) {
                resolved_pads = find_archstreamer_sdl_pads(
                    launch_plan.players,
                    config.ignore_controller.value_or(""),
                    config.verbose);
            }
            bool force_opengl = false;
            bool force_vulkan = false;
            if (config.graphics_api == GraphicsApiPreference::OpenGL) {
                force_opengl = true;
            } else if (config.graphics_api == GraphicsApiPreference::Vulkan) {
                if (virtualgl_capture) {
                    std::cerr << "Warning: VirtualGL path cannot present Vulkan; using OpenGL.\n";
                    force_opengl = true;
                } else {
                    force_vulkan = true;
                }
            } else if (virtualgl_capture) {
                force_opengl = true;
            } else if (gamescope_capture) {
                force_vulkan = true;
            }
            // Yuzu ignores gamescope --prefer-vk-device for the child and sorts Vulkan
            // devices itself (3060 before 1660). Pin qt-config vulkan_device to that order.
            int yuzu_vulkan_device = -1;
            if (resolved_gpu.has_value()) {
                yuzu_vulkan_device = yuzu_vulkan_device_index(*resolved_gpu);
            }
            const auto yuzu_user = prepare_yuzu_user_profile(
                save_profile,
                force_opengl,
                force_vulkan,
                yuzu_vulkan_device,
                config.yuzu_resolution_scale);
            std::vector<std::string> pad_guids;
            pad_guids.reserve(resolved_pads.size());
            for (const auto& pad : resolved_pads) {
                pad_guids.push_back(pad.guid);
            }
            configure_yuzu_archstreamer_controls(yuzu_user, pad_guids);
            launch_env_request.yuzu_profile = yuzu_user;
            // Always restore fullscreen launch args for standalone.
            launch_config.standalone_args_before_content = {"-f", "-g"};
            launch_config.quiet_stdio = !config.verbose;
            std::cout
                << "Yuzu renderer: "
                << (force_opengl ? "OpenGL" : force_vulkan ? "Vulkan" : "default");
            if (yuzu_vulkan_device >= 0 && resolved_gpu.has_value()) {
                std::cout
                    << " (vulkan_device=" << yuzu_vulkan_device
                    << " → " << resolved_gpu->name << ")";
            }
            std::cout << '\n';
            {
                const int scale = std::clamp(config.yuzu_resolution_scale, 1, 6);
                std::cout << "Yuzu resolution: " << scale << "x native"
                          << " (resolution_setup=" << (scale + 1) << ")\n";
            }

            if (gamescope_capture) {
#ifndef _WIN32
                const auto x_pos = config.video_resolution.find('x');
                int width = 1280;
                int height = 720;
                if (x_pos != std::string::npos) {
                    try {
                        width = std::stoi(config.video_resolution.substr(0, x_pos));
                        height = std::stoi(config.video_resolution.substr(x_pos + 1));
                    } catch (const std::exception&) {
                    }
                }
                auto prefix = gamescope_command_prefix(width, height, gamescope_vk_device);
                if (prefix.empty()) {
                    throw std::runtime_error(
                        "Switch streaming requires gamescope. Install it or set ARCHSTREAMER_GAMESCOPE "
                        "(managed: ~/.local/share/archstreamer/gamescope/archstreamer-gamescope)");
                }
                launch_config.command_prefix = std::move(prefix);
                // Always log WSI — without it the non-boot NVIDIA (3060 here) dies with
                // "Device lacks a present queue" under nested XWayland.
                std::cout
                    << "Yuzu: gamescope headless via " << launch_config.command_prefix.front()
                    << " (" << width << "x" << height
                    << ", prefer-vk-device " << gamescope_vk_device
                    << ", Gamescope WSI enabled)\n";
                for (const auto& [key, value] : gamescope_launch_environment()) {
                    if (key == "VK_ADD_IMPLICIT_LAYER_PATH") {
                        std::cout << "Gamescope WSI layer path: " << value << '\n';
                        break;
                    }
                }
#endif
            } else if (virtualgl_capture) {
#ifndef _WIN32
                auto prefix = virtual_gl_command_prefix();
                if (prefix.empty()) {
                    throw std::runtime_error(
                        "VirtualGL (vglrun) not found; install VirtualGL or set ARCHSTREAMER_VGLRUN");
                }
                launch_config.command_prefix = std::move(prefix);
                std::cout
                    << "Yuzu: VirtualGL OpenGL via " << launch_config.command_prefix.front()
                    << " (3D " << default_vgl_display()
                    << ", capture " << capture_display << ")\n";
#endif
#ifdef _WIN32
            } else if (launch_config.standalone && use_virtual_capture) {
                std::cout
                    << "Yuzu: Windows desktop capture (d3d11screencapturesrc / WASAPI loopback)\n";
#endif
            }
            std::cout
                << "Yuzu user data: " << yuzu_user.xdg_data_home
                << "\nYuzu keys:      " << yuzu_user.keys_directory << '\n';
        } else {
            const auto runtime_override = write_retroarch_input_override(
                virtual_joypad_index,
                launch_plan.virtual_identities,
                config.retroarch_joypad_driver,
                launch_plan.players,
                save_profile,
                config.audio || config.video,
                capture_fullscreen && use_virtual_capture,
                config.video_resolution,
                (!use_virtual_capture && resolved_gpu.has_value()) ? resolved_gpu->vulkan_index : -1,
                system_key,
                launch_config.core_path,
                config.retroarch_resolution_scale);
            launch_config.extra_args.push_back("-c");
            launch_config.extra_args.push_back(runtime_override.string());
            if (virtualgl_capture) {
#ifndef _WIN32
                auto prefix = virtual_gl_command_prefix();
                if (prefix.empty()) {
                    throw std::runtime_error(
                        "VirtualGL (vglrun) not found; install VirtualGL or set ARCHSTREAMER_VGLRUN");
                }
                // Wrap the resolved RetroArch argv (system binary or flatpak-spawn prefix).
                prefix.insert(
                    prefix.end(),
                    launch_config.command_prefix.begin(),
                    launch_config.command_prefix.end());
                launch_config.command_prefix = std::move(prefix);
                std::cout
                    << "RetroArch: VirtualGL via " << launch_config.command_prefix.front()
                    << " (3D on " << default_vgl_display()
                    << ", capture " << capture_display;
                if (resolved_gpu.has_value() && !resolved_gpu->prime_provider.empty()) {
                    std::cout << ", PRIME " << resolved_gpu->prime_provider;
                }
                std::cout << ")\n";
#endif
            }
            std::cout
                << "RetroArch config: " << runtime_override
                << "\nVirtual joypad index: " << virtual_joypad_index
                << " (driver=" << config.retroarch_joypad_driver << ")\n";
            {
                const int scale = std::clamp(config.retroarch_resolution_scale, 1, 6);
                std::cout << "RetroArch resolution: " << scale << "x native"
                          << " (known cores via .opt)\n";
            }
            if (!system_key.empty()) {
                std::cout << "Face buttons: system=" << system_key;
                if (system_key == "ps1" || system_key == "ps2" || system_key == "psp") {
                    std::cout << " (PlayStation position map)\n";
                } else {
                    std::cout << " (Nintendo/Xbox letter map)\n";
                }
            }
        }

        // Single composition point: audio → input → gpu → capture → emulator profile.
        {
            const auto launch_env = build_emulator_launch_environment(launch_env_request);
            launch_config.environment = launch_env.entries;
            launch_config.unset_environment = launch_env.unset;
        }

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
        auto media_index = media_destinations.size();
        if (config.audio || config.video) {
#ifdef _WIN32
            if (config.audio_backend == AudioCaptureBackend::Pulse) {
                config.audio_backend = AudioCaptureBackend::Wasapi;
            }
#endif
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
        if (session_plan.has_value()) {
            for (const auto& stream : media_streams) {
                if (stream.client_id == HostClientId) {
                    continue;
                }
                if (!stream.endpoint.video_uri.empty() || !stream.endpoint.audio_uri.empty()) {
                    send_media_endpoint_to_client(*session_plan, stream.client_id, stream.endpoint);
                }
            }
        }

        if (session_plan.has_value()) {
            send_session_starting_to_clients(*session_plan);
        }
        auto late_viewer_listener = std::optional<TcpListener>{};
        if (session_plan.has_value() && config.control_port.has_value()) {
            late_viewer_listener.emplace(*config.control_port);
            std::cout
                << "Accepting late viewers and reconnecting players on TCP port "
                << *config.control_port << ".\n";
        }
        auto session_monitor = std::optional<SessionControlMonitor>{};
        if (session_plan.has_value() && media_server) {
            session_monitor.emplace(
                *session_plan,
                input_router,
                *media_server,
                std::chrono::seconds(config.client_timeout_seconds),
                std::chrono::seconds(config.player_reconnect_timeout_seconds));
        }

        auto local_bridge = std::optional<LocalControllerBridge>{};
        if (bridge_device.has_value()) {
            local_bridge.emplace(*bridge_device);
        }

        HostRetroArchProcess retroarch;
        {
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
        retroarch.launch(launch_config);
        // Flatpak RetroArch can take a moment; failed exec exits almost immediately.
        for (int i = 0; i < 10 && retroarch.running(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (!retroarch.running()) {
            const auto code = retroarch.last_exit_code().value_or(127);
            const auto stderr_tail = retroarch.last_stderr_tail();
            if (launch_config.standalone) {
                std::string message =
                    "Standalone emulator exited immediately (code " + std::to_string(code) + "). "
                    "Check Yuzu AppImage/keys under ~/.local/share/archstreamer/yuzu and "
                    "per-user data under the save profile yuzu/ directory.";
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
#ifndef _WIN32
        if (auto* gst = dynamic_cast<GStreamerMediaServer*>(media_server.get());
            gst != nullptr && gst->video_deferred()) {
            if (config.verbose) {
                std::cout << "Waiting for gamescope PipeWire video node...\n";
            }
            int expect_w = 1280;
            int expect_h = 720;
            const auto x_pos = config.video_resolution.find('x');
            if (x_pos != std::string::npos) {
                try {
                    expect_w = std::stoi(config.video_resolution.substr(0, x_pos));
                    expect_h = std::stoi(config.video_resolution.substr(x_pos + 1));
                } catch (const std::exception&) {
                }
            }
            const auto node = wait_for_gamescope_pipewire_node(
                std::chrono::seconds(20),
                expect_w,
                expect_h);
            if (!node.has_value()) {
                throw std::runtime_error(
                    "gamescope did not publish a PipeWire Video/Source (media.name=gamescope)");
            }
            if (config.verbose) {
                std::cout << "Gamescope PipeWire node: " << *node << '\n';
            }
            gst->start_pipewire_video(*node, media_streams);
        }
#endif
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
        while (!should_stop() && retroarch.running()) {
            if (local_bridge.has_value()) {
                local_bridge->update(input_router);
            }
            if (late_viewer_listener.has_value() && session_plan.has_value() && media_server) {
                poll_active_session_joins(
                    *late_viewer_listener,
                    *session_plan,
                    list,
                    config,
                    media_index,
                    *media_server);
            }
            if (session_monitor.has_value()) {
                if (const auto reason = session_monitor->poll(); reason.has_value()) {
                    std::cerr << "Stopping session: " << *reason << '\n';
                    session_end_reason = *reason;
                    break;
                }
            }
            if (session_plan.has_value() &&
                !launch_config.standalone &&
                session_plan->link_cable.consume_relaunch_request()) {
                const auto link_core = session_plan->link_cable.pending_core_path();
                if (!link_core.has_value()) {
                    std::cerr << "Link cable relaunch requested but core path is empty\n";
                } else {
                    std::cout
                        << "Link cable: relaunching RetroArch with "
                        << link_core->string() << '\n';
                    retroarch.stop();
                    launch_config.core_path = *link_core;
                    LinkCableBackend::write_dual_gb_core_options();
                    const auto runtime_override = write_retroarch_input_override(
                        virtual_joypad_index,
                        launch_plan.virtual_identities,
                        config.retroarch_joypad_driver,
                        std::max<RetroArchPort>(launch_plan.players, 2),
                        save_profile,
                        config.audio || config.video,
                        capture_fullscreen && use_virtual_capture,
                        config.video_resolution,
                        (!use_virtual_capture && resolved_gpu.has_value())
                            ? resolved_gpu->vulkan_index
                            : -1,
                        system_key,
                        launch_config.core_path,
                        config.retroarch_resolution_scale);
                    // Replace prior -c path if present; otherwise append.
                    bool replaced_c = false;
                    for (std::size_t i = 0; i + 1 < launch_config.extra_args.size(); ++i) {
                        if (launch_config.extra_args[i] == "-c") {
                            launch_config.extra_args[i + 1] = runtime_override.string();
                            replaced_c = true;
                            break;
                        }
                    }
                    if (!replaced_c) {
                        launch_config.extra_args.push_back("-c");
                        launch_config.extra_args.push_back(runtime_override.string());
                    }
                    {
                        const auto launch_env = build_emulator_launch_environment(launch_env_request);
                        launch_config.environment = launch_env.entries;
                        launch_config.unset_environment = launch_env.unset;
                    }
                    retroarch.launch(launch_config);
                    for (int i = 0; i < 10 && retroarch.running(); ++i) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    }
                    if (!retroarch.running()) {
                        session_end_reason =
                            "link cable relaunch failed (RetroArch exited immediately)";
                        std::cerr << "Stopping session: " << *session_end_reason << '\n';
                        break;
                    }
                    send_retroarch_netcmd(
                        "SHOW_MSG Link cable active — Cable Club",
                        session_plan->retroarch_netcmd_port);
                    std::cout << "Link cable: dual-GB RetroArch is running\n";
                }
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

        if (!should_stop() && !session_end_reason.has_value() && !retroarch.running()) {
            const auto code = retroarch.last_exit_code().value_or(-1);
            const auto stderr_tail = retroarch.last_stderr_tail();
            std::ostringstream reason;
            reason << "emulator exited (code " << code << ")";
            if (launch_config.standalone && gamescope_capture) {
                reason << " — if Host GPU is the non-boot NVIDIA, check Gamescope WSI "
                          "(ENABLE_GAMESCOPE_WSI / VK_ADD_IMPLICIT_LAYER_PATH); "
                          "Yuzu often logs \"Device lacks a present queue\"";
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

        retroarch.stop();
        if (!launch_config.standalone && system_key == "ps2") {
            harvest_user_ps2_memcards(save_profile);
            std::cout
                << "PS2 memcards: harvested to "
                << user_ps2_memcard_directory(save_profile) << '\n';
        }
        if (const auto code = retroarch.last_exit_code(); code.has_value()) {
            std::cout << "RetroArch exited with code " << *code << '\n';
            if (*code == 127) {
                std::cerr
                    << "hint: exit 127 usually means the RetroArch launcher was not found. "
                    << "On Bazzite install: flatpak install flathub org.libretro.RetroArch\n";
            }
        }
        if (media_server) {
            media_server->stop();
            media_server.reset();
        }
        if (config.audio) {
            streaming_audio.restore_default_sink();
        }
        cleanup_x11_capture_runtime_dir();
        if (session_plan.has_value()) {
            const std::string end_reason = should_stop()
                ? "host stopped"
                : session_end_reason.value_or("session ended");
            send_session_ended_to_clients(*session_plan, end_reason);
            session_plan.reset();
        }

        if (!session_lobby_mode || should_stop()) {
            return 0;
        }

        std::cout
            << "Session ended"
            << (session_end_reason.has_value() ? (": " + *session_end_reason) : "")
            << ". Returning to lobby — join again with any game.\n";
        session_end_reason.reset();
        config.ignore_controller = initial_ignore_controller;
        } // for (;;) session lobby
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
