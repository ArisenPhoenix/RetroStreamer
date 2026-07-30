#include "host/active_session_slot.hpp"

#include "common/cli_common.hpp"
#include "common/participant_role.hpp"
#include "common/serialization.hpp"
#include "client/controller_backend.hpp"
#include "host/game_catalog.hpp"
#include "host/gpu_select.hpp"
#ifndef _WIN32
#include "host/gstreamer_media_server.hpp"
#include "host/virtual_display.hpp"
#endif
#include "host/host_launch_planner.hpp"
#include "host/host_session_helpers.hpp"
#include "host/launch_environment.hpp"
#include "host/link_cable_backend.hpp"
#include "host/local_controller_bridge.hpp"
#include "host/retroarch_config_writer.hpp"
#include "host/retroarch_netcmd.hpp"
#include "host/retroarch_resolve.hpp"
#include "host/session_lobby.hpp"
#include "host/standalone_emulator.hpp"
#include "host/switch_save_share.hpp"
#include "host/virtual_joypad_resolve.hpp"

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

std::mutex audio_park_mutex;

void park_game_audio_locked(StreamingAudioSink* sink, int slot_index) {
    if (sink == nullptr) {
        return;
    }
    std::lock_guard lock(audio_park_mutex);
    sink->park_game_audio_for_slot(slot_index);
}

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

} // namespace

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
    if (media_server_ != nullptr) {
        media_server_->stop();
        media_server_.reset();
    }
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
        stage_user_ps2_memcards(save_profile_);
        std::cout
            << "session slot " << slot << ": PS2 memcards staged from "
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

    use_virtual_capture_ = config.video;
    const bool capture_fullscreen = config.video;
    const std::string capture_display = config.virtual_display;
    auto display_backend = config.display_backend;
#ifndef _WIN32
    if (launch_config.standalone && use_virtual_capture_ &&
        display_backend == VirtualDisplayBackend::None) {
        display_backend = VirtualDisplayBackend::Gamescope;
    }
    if (!launch_config.standalone && use_virtual_capture_ &&
        (display_backend == VirtualDisplayBackend::None ||
         display_backend == VirtualDisplayBackend::Xvfb) &&
        core_needs_gl_on_virtual_display(launch_config.core_path) &&
        find_vglrun().has_value() && command_available("Xvfb")) {
        display_backend = VirtualDisplayBackend::VirtualGL;
    } else if (!launch_config.standalone && use_virtual_capture_ &&
               display_backend == VirtualDisplayBackend::VirtualGL &&
               !core_needs_gl_on_virtual_display(launch_config.core_path)) {
        std::cout << "session slot " << slot << ": plain Xvfb for software core (skipping VirtualGL)\n";
        display_backend = VirtualDisplayBackend::Xvfb;
    }
    if (!launch_config.standalone && use_virtual_capture_ &&
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
                << "session slot " << slot << ": capture resolution capped to 1280x720 for software core "
                << "(was " << config.video_resolution << ")\n";
            config.video_resolution = "1280x720";
        }
    }
#endif
    gamescope_capture_ =
#ifndef _WIN32
        use_virtual_capture_ && display_backend == VirtualDisplayBackend::Gamescope;
#else
        false;
#endif
    const bool virtualgl_capture =
#ifndef _WIN32
        use_virtual_capture_ && display_backend == VirtualDisplayBackend::VirtualGL;
#else
        false;
#endif

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
    }

    if (launch_config.standalone) {
        if (resolved_pads.empty()) {
            resolved_pads = find_archstreamer_sdl_pads(
                launch_plan.players,
                config.ignore_controller.value_or(""),
                config.verbose,
                product_id_base);
        }
        bool force_opengl = false;
        bool force_vulkan = false;
        if (config.graphics_api == GraphicsApiPreference::OpenGL) {
            force_opengl = true;
        } else if (config.graphics_api == GraphicsApiPreference::Vulkan) {
            if (virtualgl_capture) {
                force_opengl = true;
            } else {
                force_vulkan = true;
            }
        } else if (virtualgl_capture) {
            force_opengl = true;
        } else if (gamescope_capture_) {
            force_vulkan = true;
        }

        const auto core_name = launch_config.core_path.filename().string();
        const bool use_ryujinx =
            core_name.find("Ryujinx") != std::string::npos ||
            core_name.find("ryujinx") != std::string::npos;

        if (use_ryujinx) {
            const auto ryujinx_user = prepare_ryujinx_user_profile(
                save_profile_,
                /*enable_ldn_mitm=*/true,
                config.yuzu_resolution_scale);
            launch_env_request.ryujinx_profile = ryujinx_user;
            launch_config.standalone_args_before_content = {"--fullscreen"};
            launch_config.quiet_stdio = !config.verbose;
            const auto synced = sync_switch_shared_saves_for_profile(save_profile_);
            std::cout
                << "session slot " << slot << ": Ryujinx (ldn_mitm)"
                << " config=" << ryujinx_user.data_root
                << " shared_saves=" << synced.size() << '\n';
        } else {
            int yuzu_vulkan_device = -1;
            if (resolved_gpu.has_value()) {
                yuzu_vulkan_device = yuzu_vulkan_device_index(*resolved_gpu);
            }
            const auto yuzu_user = prepare_yuzu_user_profile(
                save_profile_,
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
            launch_config.standalone_args_before_content = {"-f", "-g"};
            launch_config.quiet_stdio = !config.verbose;
            const auto synced = sync_switch_shared_saves_for_profile(save_profile_);
            if (!synced.empty()) {
                std::cout
                    << "session slot " << slot << ": Yuzu fallback; synced "
                    << synced.size() << " Switch save title(s)\n";
            }
        }

        if (gamescope_capture_) {
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
                    "Switch streaming requires gamescope. Install it or set ARCHSTREAMER_GAMESCOPE");
            }
            launch_config.command_prefix = std::move(prefix);
            std::cout
                << "session slot " << slot << ": Switch gamescope headless ("
                << width << "x" << height << ")\n";
#endif
        } else if (virtualgl_capture) {
#ifndef _WIN32
            auto prefix = virtual_gl_command_prefix();
            if (prefix.empty()) {
                throw std::runtime_error(
                    "VirtualGL (vglrun) not found; install VirtualGL or set ARCHSTREAMER_VGLRUN");
            }
            launch_config.command_prefix = std::move(prefix);
#endif
        }
    } else {
        const auto runtime_override = write_retroarch_input_override(
            virtual_joypad_index_,
            launch_plan.virtual_identities,
            config.retroarch_joypad_driver,
            launch_plan.players,
            save_profile_,
            config.audio || config.video,
            capture_fullscreen && use_virtual_capture_,
            config.video_resolution,
            (!use_virtual_capture_ && resolved_gpu.has_value()) ? resolved_gpu->vulkan_index : -1,
            system_key_,
            launch_config.core_path,
            config.retroarch_resolution_scale,
            slot,
            plan.retroarch_netcmd_port);
        launch_config.extra_args.push_back("-c");
        launch_config.extra_args.push_back(runtime_override.string());
        if (virtualgl_capture) {
#ifndef _WIN32
            auto prefix = virtual_gl_command_prefix();
            if (prefix.empty()) {
                throw std::runtime_error(
                    "VirtualGL (vglrun) not found; install VirtualGL or set ARCHSTREAMER_VGLRUN");
            }
            prefix.insert(
                prefix.end(),
                launch_config.command_prefix.begin(),
                launch_config.command_prefix.end());
            launch_config.command_prefix = std::move(prefix);
#endif
        }
    }

    {
        const auto launch_env = build_emulator_launch_environment(launch_env_request);
        launch_config.environment = launch_env.entries;
        launch_config.unset_environment = launch_env.unset;
    }

    if (config.audio) {
        park_game_audio_locked(config_.streaming_audio, slot);
    }

    input_router_ = std::make_unique<InputRouter>(*gamepads_, keyboard_.get());
    input_router_->set_seat_assignment(launch_plan.seats);

    if (config.audio || config.video) {
#ifdef _WIN32
        if (config.audio_backend == AudioCaptureBackend::Pulse) {
            config.audio_backend = AudioCaptureBackend::Wasapi;
        }
#endif
        media_server_ = make_host_media_server(GStreamerMediaCaptureConfig{
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
        media_index_ = media_destinations.size();
        media_server_->start(media_config, media_destinations, media_streams);
    }

    if (use_virtual_capture_ && !gamescope_capture_) {
        bool keyboard_ready = false;
        for (int attempt = 0; attempt < 20; ++attempt) {
            try {
                keyboard_->plug();
                keyboard_ready = true;
                break;
            } catch (const std::exception& error) {
                if (attempt == 19) {
                    std::cerr
                        << "session slot " << slot << ": warning: virtual keyboard unavailable: "
                        << error.what() << '\n';
                } else {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
            }
        }
    } else if (!use_virtual_capture_) {
        try {
            keyboard_->plug();
        } catch (const std::exception& error) {
            std::cerr
                << "session slot " << slot << ": warning: virtual keyboard unavailable: "
                << error.what() << '\n';
        }
    }

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
        session_monitor_.emplace(
            plan,
            *input_router_,
            *media_server_,
            std::chrono::seconds(config.client_timeout_seconds),
            std::chrono::seconds(config.player_reconnect_timeout_seconds),
            config_.hub);
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

    session_runtime_->start_emulator();
    for (int i = 0; i < 10 && session_runtime_->emulator_running(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (!session_runtime_->emulator_running()) {
        const auto code = session_runtime_->last_exit_code().value_or(127);
        const auto stderr_tail = session_runtime_->last_stderr_tail();
        std::string message = session_runtime_->launch_config().standalone
            ? "Standalone emulator exited immediately (code " + std::to_string(code) + ")"
            : "RetroArch exited immediately (code " + std::to_string(code) + ")";
        if (!stderr_tail.empty()) {
            message += "\n\n" + stderr_tail;
        }
        throw std::runtime_error(message);
    }

#ifndef _WIN32
    if (auto* gst = dynamic_cast<GStreamerMediaServer*>(media_server_.get());
        gst != nullptr && gst->video_deferred()) {
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
        gst->start_pipewire_video(*node, media_streams);
    }
#endif

    if (config.audio) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        park_game_audio_locked(config_.streaming_audio, slot);
    }

    if (config.pulse_input && launch_plan.players > 0) {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        pulse_virtual_pad_a(*gamepads_);
    }

    register_input_clients();

    std::optional<std::string> session_end_reason;
    auto next_audio_park = std::chrono::steady_clock::now();
    while (!should_stop() && session_runtime_->emulator_running()) {
        if (local_bridge.has_value()) {
            local_bridge->update(*input_router_);
        }
        drain_pending_joins();
        if (session_monitor_.has_value()) {
            if (const auto reason = session_monitor_->poll(); reason.has_value()) {
                std::cerr << "session slot " << slot << ": stopping: " << *reason << '\n';
                session_end_reason = *reason;
                break;
            }
        }
        if (plan.pending_link_promotion) {
            plan.pending_link_promotion = false;
            LinkPromotionRequest promotion;
            promotion.logical_host_client_id = plan.pending_link_host_client_id;
            promotion.logical_client_client_id = plan.pending_link_client_client_id;
            promotion.logical_host_username = plan.pending_link_host_username;
            promotion.logical_client_username = plan.pending_link_client_username;
            promotion.system_key = plan.system_key;

            auto link_runtime = promote_to_link_runtime(std::move(session_runtime_), std::move(promotion));
            if (!link_runtime) {
                session_end_reason = "link promotion failed";
                break;
            }
            std::cout
                << "session slot " << slot << ": link runtime "
                << link_runtime->kind_name() << '\n';
            send_retroarch_netcmd(
                "SHOW_MSG Link runtime: peer instance pending",
                plan.retroarch_netcmd_port);
            session_runtime_ = std::move(link_runtime);
        }
        if (!session_runtime_->launch_config().standalone &&
            plan.link_cable.consume_relaunch_request()) {
#if defined(ARCHSTREAMER_DEBUG_GB_LINK)
            const auto link_core = plan.link_cable.pending_core_path();
            if (link_core.has_value()) {
                session_runtime_->stop_emulator();
                auto& relaunch_config = session_runtime_->launch_config();
                relaunch_config.core_path = *link_core;
                LinkCableBackend::write_dual_gb_core_options();
                const auto runtime_override = write_retroarch_input_override(
                    virtual_joypad_index_,
                    launch_plan.virtual_identities,
                    config.retroarch_joypad_driver,
                    std::max<RetroArchPort>(launch_plan.players, 2),
                    save_profile_,
                    config.audio || config.video,
                    capture_fullscreen && use_virtual_capture_,
                    config.video_resolution,
                    (!use_virtual_capture_ && resolved_gpu.has_value()) ? resolved_gpu->vulkan_index : -1,
                    system_key_,
                    relaunch_config.core_path,
                    config.retroarch_resolution_scale,
                    slot,
                    plan.retroarch_netcmd_port);
                bool replaced_c = false;
                for (std::size_t i = 0; i + 1 < relaunch_config.extra_args.size(); ++i) {
                    if (relaunch_config.extra_args[i] == "-c") {
                        relaunch_config.extra_args[i + 1] = runtime_override.string();
                        replaced_c = true;
                        break;
                    }
                }
                if (!replaced_c) {
                    relaunch_config.extra_args.push_back("-c");
                    relaunch_config.extra_args.push_back(runtime_override.string());
                }
                {
                    const auto launch_env = build_emulator_launch_environment(launch_env_request);
                    relaunch_config.environment = launch_env.entries;
                    relaunch_config.unset_environment = launch_env.unset;
                }
                session_runtime_->start_emulator();
                for (int i = 0; i < 10 && session_runtime_->emulator_running(); ++i) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
                if (!session_runtime_->emulator_running()) {
                    session_end_reason = "link cable relaunch failed (RetroArch exited immediately)";
                    break;
                }
                send_retroarch_netcmd(
                    "SHOW_MSG Link cable active — Cable Club",
                    plan.retroarch_netcmd_port);
            }
#else
            std::cerr << "session slot " << slot << ": link cable relaunch ignored (debug off)\n";
#endif
        }
        if (const auto gba = consume_gba_netplay_relaunch(); gba.has_value()) {
            if (session_runtime_->launch_config().standalone) {
                std::cerr
                    << "session slot " << slot
                    << ": GBA netplay ignored (standalone emulator)\n";
            } else {
                // Client connects after host has time to bind --host.
                if (!gba->is_host) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(2000));
                }
                std::cout
                    << "session slot " << slot << ": GBA netplay relaunch as "
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
                const auto runtime_override = write_retroarch_input_override(
                    virtual_joypad_index_,
                    launch_plan.virtual_identities,
                    config.retroarch_joypad_driver,
                    launch_plan.players,
                    save_profile_,
                    config.audio || config.video,
                    capture_fullscreen && use_virtual_capture_,
                    config.video_resolution,
                    (!use_virtual_capture_ && resolved_gpu.has_value()) ? resolved_gpu->vulkan_index : -1,
                    system_key_,
                    relaunch_config.core_path,
                    config.retroarch_resolution_scale,
                    slot,
                    plan.retroarch_netcmd_port);
                bool replaced_c = false;
                for (std::size_t i = 0; i + 1 < relaunch_config.extra_args.size(); ++i) {
                    if (relaunch_config.extra_args[i] == "-c") {
                        relaunch_config.extra_args[i + 1] = runtime_override.string();
                        replaced_c = true;
                        break;
                    }
                }
                if (!replaced_c) {
                    relaunch_config.extra_args.push_back("-c");
                    relaunch_config.extra_args.push_back(runtime_override.string());
                }
                {
                    const auto launch_env = build_emulator_launch_environment(launch_env_request);
                    relaunch_config.environment = launch_env.entries;
                    relaunch_config.unset_environment = launch_env.unset;
                }
                session_runtime_->start_emulator();
                for (int i = 0; i < 20 && session_runtime_->emulator_running(); ++i) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
                if (!session_runtime_->emulator_running()) {
                    session_end_reason =
                        "GBA netplay relaunch failed (RetroArch exited immediately)";
                    break;
                }
                send_retroarch_netcmd(
                    gba->is_host
                        ? "SHOW_MSG GBA cable host ready — Cable Club"
                        : "SHOW_MSG GBA cable connected — Cable Club",
                    plan.retroarch_netcmd_port);
            }
        }
        if (config.audio) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= next_audio_park) {
                park_game_audio_locked(config_.streaming_audio, slot);
                next_audio_park = now + std::chrono::seconds(3);
            }
        }

        if (local_bridge.has_value()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    if (!should_stop() && !session_end_reason.has_value() && !session_runtime_->emulator_running()) {
        const auto code = session_runtime_->last_exit_code().value_or(-1);
        std::ostringstream reason;
        reason << "emulator exited (code " << code << ")";
        session_end_reason = reason.str();
    }

    session_runtime_->stop_emulator();
    if (!session_runtime_->launch_config().standalone && system_key_ == "ps2") {
        harvest_user_ps2_memcards(save_profile_);
    }

    const std::string end_reason = should_stop()
        ? "host stopped"
        : session_end_reason.value_or("session ended");
    shutdown_media_and_clients(end_reason);
}

} // namespace archstreamer
