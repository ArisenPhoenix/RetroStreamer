#include "common/catalog_presenter.hpp"
#include "common/cli_common.hpp"
#include "common/participant_role.hpp"
#include "common/platform/default_platform.hpp"
#include "host/game_catalog_scanner.hpp"
#include "host/gstreamer_media_server.hpp"
#include "host/host_app.hpp"
#include "host/host_app_config.hpp"
#include "host/host_launch_planner.hpp"
#include "host/host_session_helpers.hpp"
#include "host/input_router.hpp"
#include "host/local_controller_bridge.hpp"
#include "host/network_input_receiver.hpp"
#include "host/platform/default_host_platform.hpp"
#include "host/retroarch_config_writer.hpp"
#include "host/save_profile.hpp"
#include "host/session_control_monitor.hpp"
#include "host/session_service.hpp"

#include <chrono>
#include <iostream>
#include <optional>
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

        if (config.audio) {
            config.audio_backend = choose_audio_capture_backend(config.audio_backend);
            if (config.audio_source.empty()) {
                config.audio_source = default_audio_monitor_source();
                if (config.audio_source.empty()) {
                    std::cerr
                        << "Warning: could not determine default audio monitor source; "
                        << "audio capture will use the audio server default source.\n";
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
        if (config.host_role == ParticipantRole::Player && config.bridge_controller_index.has_value()) {
            bridge_device = local_bridge_device_for(*config.bridge_controller_index);
            if (config.retroarch_joypad_driver == "udev" && !config.ignore_controller.has_value()) {
                config.retroarch_joypad_driver = "sdl2";
            }
        }
        auto bridge_identity = std::optional<HostPlayerControllerIdentity>{};
        if (bridge_device.has_value()) {
            bridge_identity = host_player_controller_identity(*bridge_device);
        }

        auto launch_plan = HostLaunchPlan{};
        auto session_plan = std::optional<SessionPlan>{};
        if (config.control_port.has_value()) {
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
                should_stop);
            session_plan = session_service.wait_for_ready_session();
            if (config.retroarch_joypad_driver == "udev" && !config.ignore_controller.has_value()) {
                config.retroarch_joypad_driver = "sdl2";
            }
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
        launch_config.extra_args.insert(launch_config.extra_args.begin(), "-f");
        if (config.verbose) {
            launch_config.extra_args.insert(launch_config.extra_args.begin(), "--verbose");
        }
        const auto default_joypad_index =
            (config.bridge_controller_index.has_value() || config.control_port.has_value()) &&
                config.retroarch_joypad_driver == "sdl2" ? 0 : 1;
        const auto virtual_joypad_index = config.virtual_joypad_index.value_or(default_joypad_index);
        const auto runtime_override = write_retroarch_input_override(
            virtual_joypad_index,
            launch_plan.virtual_identities,
            config.retroarch_joypad_driver,
            launch_plan.players,
            save_profile,
            config.audio || config.video);
        launch_config.extra_args.push_back("-c");
        launch_config.extra_args.push_back(runtime_override.string());
        if (config.audio || config.video) {
            launch_config.environment.emplace_back("SDL_AUDIODRIVER", "pulse");
        }
        if (config.audio) {
            if (!config.audio_source.empty() && config.audio_source.ends_with(".monitor")) {
                launch_config.environment.emplace_back(
                    "PULSE_SINK",
                    config.audio_source.substr(0, config.audio_source.size() - std::string(".monitor").size()));
            }
        }
        if (bridge_device.has_value() && !config.ignore_controller.has_value()) {
            if (bridge_device->vendor_id != 0 && bridge_device->product_id != 0) {
                config.ignore_controller = hex_vid_pid(bridge_device->vendor_id, bridge_device->product_id);
            }
        }
        if (config.ignore_controller.has_value()) {
            launch_config.environment.emplace_back("SDL_GAMECONTROLLER_IGNORE_DEVICES", *config.ignore_controller);
            if (config.retroarch_joypad_driver != "sdl2") {
                std::cerr
                    << "Warning: SDL_GAMECONTROLLER_IGNORE_DEVICES only affects RetroArch when "
                    << "--retroarch-joypad-driver is sdl2.\n";
            }
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
        if (config.video) {
            // Keep RetroArch on the real display when the host is playing locally.
            // Streaming still uses a virtual capture display for remote clients only.
            const bool host_plays_locally =
                config.host_role == ParticipantRole::Player &&
                config.bridge_controller_index.has_value();
            if (!host_plays_locally) {
                launch_config.environment.emplace_back("DISPLAY", config.virtual_display);
            } else {
                std::cout
                    << "Host player keeps the current DISPLAY; "
                    << "disable Stream video if remotes do not need a capture feed.\n";
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
                    << " from display " << config.virtual_display
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
                    << (config.audio_backend == AudioCaptureBackend::PipeWire ? "pipewire" : "pulse")
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
            std::cout << "Ignoring:  " << *config.ignore_controller << " for SDL2 controller discovery\n";
        }

        if (config.dry_run) {
            if (session_plan.has_value()) {
                for (const auto& stream : media_streams) {
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
        std::this_thread::sleep_for(std::chrono::milliseconds(750));

        InputRouter input_router(gamepads);
        input_router.set_seat_assignment(launch_plan.seats);

        auto network_receiver = std::optional<NetworkInputReceiver>{};
        if (config.input_port.has_value()) {
            network_receiver.emplace(*config.input_port, input_router);
        }

        auto media_server = std::unique_ptr<MediaServer>{};
        auto media_index = media_destinations.size();
        if (config.video || config.audio) {
            media_server = make_host_media_server(GStreamerMediaCaptureConfig{
                config.video,
                config.audio,
                config.virtual_display,
                config.video_resolution,
                config.display_backend,
                config.audio_backend,
                config.audio_source,
            });
            media_server->start(media_config, media_destinations, media_streams);
        }
        if (session_plan.has_value()) {
            for (const auto& stream : media_streams) {
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
        retroarch.launch(launch_config);

        if (config.pulse_input && launch_plan.players > 0) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            pulse_virtual_pad_a(gamepads);
        }

        while (!should_stop() && retroarch.running()) {
            if (local_bridge.has_value()) {
                local_bridge->update(input_router);
            }
            if (network_receiver.has_value()) {
                network_receiver->poll();
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
                    break;
                }
            }

            if (local_bridge.has_value() || network_receiver.has_value()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(8));
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
            }
        }

        retroarch.stop();
        if (media_server) {
            media_server->stop();
        }
        if (session_plan.has_value()) {
            send_session_ended_to_clients(*session_plan, should_stop() ? "host stopped" : "retroarch exited");
        }
        return 0;
    } catch (const std::exception& error) {
        if (should_stop()) {
            std::cout << "Host stopped.\n";
            return 0;
        }
        std::cerr << "host_runner: " << error.what() << '\n';
        return 1;
    }
}

} // namespace archstreamer
