#include "host/session_launch_types.hpp"

#include "client/controller_backend.hpp"
#include "common/cli_common.hpp"
#include "common/steam_art_import.hpp"
#include "host/host_session_helpers.hpp"
#include "host/session_launch_assemble.hpp"
#include "host/session_lobby.hpp"
#include "host/standalone_emulator.hpp"
#include "host/switch_save_share.hpp"
#include "host/virtual_display.hpp"

#include <chrono>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>

namespace archstreamer {
namespace {

constexpr const char* kSteamInputIgnoreDevices =
    "0x28de/0x11ff,0x28de/0x1205,0x28de/0x1201";

void append_ignore_devices(HostAppConfig& config, const std::string& devices) {
    if (devices.empty()) {
        return;
    }
    if (config.ignore_controller.has_value() && !config.ignore_controller->empty()) {
        config.ignore_controller = *config.ignore_controller + "," + devices;
    } else {
        config.ignore_controller = devices;
    }
}

} // namespace

std::string SessionDevicePlan::capture_info() const {
    std::ostringstream o;
    o << "Capture: " << (capture_fullscreen ? "fullscreen" : "windowed") << video_resolution
      << " on display " << capture_display
      << (use_virtual_capture ? " (virtual)" : " (host)") << '\n';
    return o.str();
}

void apply_capture_to_session_device_plan(
    SessionDevicePlan& devices,
    const HostAppConfig& config,
    const CapturePlan& capture,
    std::optional<GpuDevice> resolved_gpu) {
    devices.resolved_gpu = std::move(resolved_gpu);
    devices.use_virtual_capture = capture.use_virtual_capture;
    devices.capture_fullscreen = capture.capture_fullscreen;
    devices.capture_display = capture.capture_display;
    devices.display_backend = capture.display_backend;
    devices.video_resolution = config.video_resolution;
}

SessionDevicePlan resolve_session_device_plan(
    const HostAppConfig& config,
    const HostLaunchPlan& launch_plan,
    SessionPadPlanKind kind,
    std::uint16_t product_id_base) {
    SessionDevicePlan devices;
    devices.product_id_base = product_id_base;
    const bool use_udev = config.retroarch_joypad_driver == "udev";
    if (config.verbose && use_udev && kind == SessionPadPlanKind::Direct) {
        std::cout << "udev joysticks (ArchStreamer hunt):\n";
    }
    if (kind == SessionPadPlanKind::RetroArchSlot) {
        devices.shared_pad_plan = resolve_retroarch_slot_pad_plan(
            launch_plan.players,
            config.ignore_controller.value_or(""),
            config.verbose,
            product_id_base,
            use_udev);
    } else {
        devices.shared_pad_plan = resolve_shared_pad_plan(
            launch_plan.players,
            config.ignore_controller.value_or(""),
            config.verbose,
            product_id_base,
            use_udev);
    }

    if (use_udev) {
        devices.resolved_indices = devices.shared_pad_plan.udev_indices;
        devices.resolved_pads = devices.shared_pad_plan.pads;
    } else {
        devices.resolved_pads = devices.shared_pad_plan.pads;
        devices.resolved_indices.reserve(devices.resolved_pads.size());
        for (const auto& pad : devices.resolved_pads) {
            devices.resolved_indices.push_back(pad.sdl_index);
        }
    }
    if (config.virtual_joypad_index.has_value()) {
        devices.virtual_joypad_index = *config.virtual_joypad_index;
        if (kind == SessionPadPlanKind::Direct) {
            std::cout
                << "Using explicit --virtual-joypad-index "
                << devices.virtual_joypad_index << '\n';
        }
    } else if (!devices.resolved_indices.empty()) {
        devices.virtual_joypad_index = devices.resolved_indices.front();
        if (kind == SessionPadPlanKind::Direct && config.verbose) {
            std::cout
                << "Resolved virtual joypad index " << devices.virtual_joypad_index
                << " (driver=" << config.retroarch_joypad_driver << ")\n";
        }
    } else if (kind == SessionPadPlanKind::Direct) {
        std::cerr
            << "Warning: ArchStreamer virtual pads not visible to "
            << config.retroarch_joypad_driver
            << " yet; defaulting RetroArch joypad index to 0.\n";
    }
    return devices;
}

SessionMediaPlan build_session_media_plan(
    const HostAppConfig& config,
    SessionPlan* session_plan,
    SessionMediaPlanKind kind) {
    SessionMediaPlan media;
    if (kind == SessionMediaPlanKind::Slot) {
        if (session_plan == nullptr) {
            throw std::invalid_argument("slot media plan requires a SessionPlan");
        }
        parse_video_resolution(config.video_resolution, media.capture_w, media.capture_h);
        configure_initial_session_video(*session_plan, media.capture_w, media.capture_h);
    }
    media.config = media_plan_config_for(config);
    if (kind == SessionMediaPlanKind::Slot) {
        media.config.initial_video_settings = session_plan->session_video_settings;
    }
    if (config.video || config.audio) {
        if (kind == SessionMediaPlanKind::Slot) {
            media.destinations = media_destinations_for_session(media.config, *session_plan);
        } else {
            media.destinations = media_destinations_for_host(media.config);
        }
        media.streams = media_streams_for_dry_run(media.config, media.destinations);
    }
    return media;
}

SessionContentInfo resolve_session_content_info(
    GameCatalog& catalog,
    const HostLaunchPlan& launch_plan) {
    SessionContentInfo content;
    if (const auto hosted = catalog.find_hosted(launch_plan.game_id); hosted.has_value()) {
        auto hosted_game = hosted->get();
        content.system_key = hosted_game.info.system_key;
        content.active_display_name = hosted_game.info.display_name;
        content.catalog_content_path = hosted_game.content_path;
        content.m3m_title_id = hosted_game.m3m_title_id;
        content.playlist_discs = hosted_game.info.playlist_discs;
    } else if (const auto info = catalog.find(launch_plan.game_id); info.has_value()) {
        content.system_key = info->system_key;
        content.active_display_name = info->display_name;
        content.playlist_discs = info->playlist_discs;
    }
    return content;
}

SessionLaunchAssets prepare_session_launch_assets(
    HostAppConfig& config,
    GameCatalog& catalog,
    const HostLaunchPlan& launch_plan,
    std::string_view log_prefix) {
    auto assets = SessionLaunchAssets{};
    assets.save_profile = prepare_save_profile(config.save_root, launch_plan.save_username);
    assets.launch_config = catalog.launch_config_for(launch_plan.game_id);
    assets.resolved_retroarch = resolve_retroarch();
    assets.content = resolve_session_content_info(catalog, launch_plan);
    apply_session_content_launch_adjustments(
        config,
        assets.launch_config,
        assets.save_profile,
        assets.resolved_retroarch,
        assets.content,
        log_prefix);
    return assets;
}

SessionLaunchContext prepare_session_launch_context(
    HostAppConfig& config,
    GameCatalog& catalog,
    HostLaunchPlan launch_plan,
    std::string_view log_prefix) {
    auto context = SessionLaunchContext{};
    context.launch_plan = std::move(launch_plan);
    context.assets = prepare_session_launch_assets(
        config,
        catalog,
        context.launch_plan,
        log_prefix);
    return context;
}

void apply_session_content_launch_adjustments(
    HostAppConfig& config,
    RetroArchLaunchConfig& launch_config,
    const SaveProfile& save_profile,
    const ResolvedRetroArch& resolved_retroarch,
    const SessionContentInfo& content,
    std::string_view log_prefix) {
    if (!launch_config.standalone && content.system_key == "ps2") {
        std::cout
            << log_prefix
            << "PS2 memcards: " << user_ps2_memcard_directory(save_profile) << '\n';
    }

#if !defined(_WIN32)
    if (!launch_config.standalone &&
        (content.system_key == "ps1" || content.system_key == "ps2" ||
         content.system_key == "psp") &&
        config.retroarch_joypad_driver == "sdl2") {
        if (log_prefix.empty()) {
            std::cout
                << "Note: forcing joypad driver udev for " << content.system_key
                << " (sdl2 stalls PlayStation cores).\n";
        } else {
            std::cout
                << log_prefix << "forcing joypad driver udev for " << content.system_key
                << " (sdl2 stalls PlayStation cores).\n";
        }
        config.retroarch_joypad_driver = "udev";
    }
#endif

    if (!launch_config.standalone) {
        launch_config.retroarch_path = resolved_retroarch.display_path;
        launch_config.command_prefix = resolved_retroarch.argv_prefix;
    }
    if (config.verbose && !launch_config.standalone) {
        launch_config.extra_args.insert(launch_config.extra_args.begin(), "--verbose");
    }
}

void prepare_session_standalone_backend(
    std::string_view system_key,
    RetroArchLaunchConfig& launch_config,
    SessionBackendState& backends) {
    if (system_key == "switch") {
        const auto runtime = resolve_switch_runtime();
        if (!runtime.has_value()) {
            throw std::runtime_error(switch_runtime_unavailable_message());
        }
        launch_config.standalone = true;
        launch_config.core_path = runtime->path;
        launch_config.standalone_args_before_content = runtime->args_before_content;
        backends.switch_backend = make_switch_backend(*runtime);
    } else if (system_key == "nds" && melonds_runtime_available()) {
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
            "standalone launch requested for unsupported system_key=" +
            std::string(system_key));
    }
}

SwitchLaunchContent resolve_switch_launch_content(
    const SaveProfile& save_profile,
    const RetroArchLaunchConfig& launch_config,
    const SessionContentInfo& content) {
    SwitchLaunchContent switch_content;
    switch_content.content_stem = !content.catalog_content_path.empty()
        ? content.catalog_content_path.stem().string()
        : launch_config.content_path.stem().string();
    switch_content.title_id = content.m3m_title_id;
    if (switch_content.title_id.empty()) {
        switch_content.title_id = resolve_switch_title_id_for_catalog(
            save_profile,
            switch_content.content_stem,
            launch_config.content_path);
    }
    return switch_content;
}

SessionParticipantContext resolve_direct_session_participants(
    std::string_view save_username) {
    SessionParticipantContext participants;
    participants.profile_display_name = preferred_steam_or_username_display_name(save_username);
    participants.display_layout = DisplayLayoutPreference::Auto;
    return participants;
}

SessionParticipantContext resolve_session_plan_participants(
    std::string_view save_username,
    const SessionPlan& plan) {
    SessionParticipantContext participants;
    participants.client_hellos.reserve(plan.clients.size());
    for (const auto& client : plan.clients) {
        participants.client_hellos.push_back(client.hello);
    }
    participants.profile_display_name = resolve_switch_profile_display_name(
        save_username,
        plan.host_hello,
        participants.client_hellos);
    participants.display_layout =
        resolve_display_layout_preference(plan.host_hello, participants.client_hellos);
    return participants;
}

void plug_session_gamepads(VirtualGamepadBus& gamepads, RetroArchPort players) {
    for (RetroArchPort port = 0; port < players; ++port) {
        gamepads.plug(port);
    }
}

void wait_for_session_input_enumeration() {
    std::this_thread::sleep_for(std::chrono::milliseconds(750));
}

SessionGpuSelection resolve_session_gpu_selection(
    const HostAppConfig& config,
    const CapturePlan& capture,
    std::string_view log_prefix,
    bool detailed_logging) {
    SessionGpuSelection selection;
    selection.resolved_encode = resolve_render_gpu(config.encode_gpu);
    selection.resolved_gpu = resolve_render_gpu(effective_render_gpu_selection(config));
    if (selection.resolved_encode.has_value() && selection.resolved_encode->nvidia_index >= 0) {
        selection.nvenc_cuda_device_id = selection.resolved_encode->nvidia_index;
    }
    if (selection.resolved_gpu.has_value()) {
        const auto gpu_label = [&]() {
            return log_prefix.empty() && detailed_logging
                ? std::string("GPU: ")
                : std::string(log_prefix) + "GPU ";
        };
        const auto encode_label = [&]() {
            return log_prefix.empty() && detailed_logging
                ? std::string("Encode GPU: ")
                : std::string(log_prefix) + "encode GPU ";
        };
        const auto render_label = [&]() {
            return log_prefix.empty() && detailed_logging
                ? std::string("Render GPU: ")
                : std::string(log_prefix) + "render GPU ";
        };
        const bool same_as_encode =
            selection.resolved_encode.has_value() &&
            selection.resolved_encode->id == selection.resolved_gpu->id;
        if (same_as_encode) {
            std::cout
                << gpu_label() << selection.resolved_gpu->name
                << " [" << selection.resolved_gpu->id << "] (encode+render)";
        } else {
            if (selection.resolved_encode.has_value()) {
                std::cout
                    << encode_label() << selection.resolved_encode->name
                    << " [" << selection.resolved_encode->id << "]";
                if (detailed_logging && selection.resolved_encode->nvidia_index >= 0) {
                    std::cout << " nvidia_index=" << selection.resolved_encode->nvidia_index;
                }
                std::cout << '\n';
            }
            std::cout
                << render_label() << selection.resolved_gpu->name
                << " [" << selection.resolved_gpu->id << "]";
            if (detailed_logging && config.separate_render_gpu) {
                std::cout << " (separate from encode)";
            }
        }
        if (detailed_logging && selection.resolved_gpu->vulkan_index >= 0) {
            std::cout << " vulkan_index=" << selection.resolved_gpu->vulkan_index;
        }
        if (detailed_logging && !selection.resolved_gpu->prime_provider.empty()) {
            std::cout << " prime=" << selection.resolved_gpu->prime_provider;
        }
        std::cout << '\n';
        if (const auto vd = pci_vendor_device_id(selection.resolved_gpu->pci_bus); vd.has_value()) {
            selection.gamescope_vk_device = *vd;
        }
        if (detailed_logging &&
            selection.resolved_gpu->prime_provider.empty() &&
            selection.resolved_gpu->nvidia_index >= 0) {
            std::cerr
                << "Warning: NVIDIA GPU selected but no PRIME provider was mapped "
                << "(is DISPLAY set when scanning GPUs?). Capture GL may use llvmpipe.\n";
        }
        if (detailed_logging &&
            capture.use_virtual_capture && !capture.virtualgl_capture &&
            !capture.gamescope_capture && selection.resolved_gpu->nvidia_index > 0) {
            std::cerr
                << "Warning: streamed OpenGL on plain Xvfb cannot select NVIDIA GPU index "
                << selection.resolved_gpu->nvidia_index
                << " (always uses nvidia:0). Install VirtualGL (vglrun) so Host GPU works.\n";
        }
    } else if (selection.resolved_encode.has_value()) {
        const auto encode_label = log_prefix.empty() && detailed_logging
            ? std::string("Encode GPU: ")
            : std::string(log_prefix) + "encode GPU ";
        std::cout
            << encode_label << selection.resolved_encode->name
            << " [" << selection.resolved_encode->id << "]";
        if (detailed_logging && selection.resolved_encode->nvidia_index >= 0) {
            std::cout << " nvidia_index=" << selection.resolved_encode->nvidia_index;
        }
        std::cout << '\n';
    }
    if (selection.gamescope_vk_device.empty()) {
        selection.gamescope_vk_device = "10de:2504";
    }
    return selection;
}

void apply_session_gpu_to_launch_request(
    EmulatorLaunchEnvRequest& request,
    const SessionGpuSelection& selection) {
    if (selection.resolved_gpu.has_value()) {
        request.render_gpu = *selection.resolved_gpu;
    }
}

void append_session_controller_ignore_list(
    HostAppConfig& config,
    const std::optional<ControllerDevice>& bridge_device,
    std::string_view log_prefix,
    bool warn_if_not_sdl2) {
    if (bridge_device.has_value() && !config.ignore_controller.has_value()) {
        if (bridge_device->vendor_id != 0 && bridge_device->product_id != 0) {
            config.ignore_controller = hex_vid_pid(
                bridge_device->vendor_id,
                bridge_device->product_id);
        }
    }

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
        append_ignore_devices(config, host_ignore);
    } catch (const std::exception& error) {
        if (log_prefix.empty()) {
            std::cerr
                << "Warning: host controller scan for ignore list failed: "
                << error.what() << '\n';
        } else {
            std::cerr
                << log_prefix
                << "warning: host controller scan failed: "
                << error.what() << '\n';
        }
    }

    append_ignore_devices(config, kSteamInputIgnoreDevices);

    if (warn_if_not_sdl2 && config.retroarch_joypad_driver != "sdl2") {
        std::cerr
            << "Warning: SDL_GAMECONTROLLER_IGNORE_DEVICES only affects RetroArch when "
            << "--retroarch-joypad-driver is sdl2.\n";
    }
}

} // namespace archstreamer
