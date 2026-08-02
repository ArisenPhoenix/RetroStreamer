#include "host/standalone_emulator.hpp"

#include "host/switch/default_switch_platform.hpp"
#include "host/switch/ryujinx_controls.hpp"
#include "host/switch/ryujinx_launch_env.hpp"
#include "host/switch/ryujinx_user_profile.hpp"
#include "host/switch/switch_runtime.hpp"
#include "host/switch/yuzu_controls.hpp"
#include "host/switch/yuzu_launch_env.hpp"
#include "host/switch/yuzu_user_profile.hpp"
#include "host/switch_save_share.hpp"

#include <iostream>

namespace archstreamer {

std::filesystem::path default_archstreamer_data_root() {
    return SwitchPaths::archstreamer_data_root();
}

std::filesystem::path switch_system_defaults_root() {
    return SwitchPaths::archstreamer_data_root() / "system" / "switch";
}

std::filesystem::path default_yuzu_runtime_root() {
    return YuzuRuntime::runtime_root();
}

bool yuzu_runtime_available() {
    return YuzuRuntime::available();
}

std::string yuzu_unavailable_message() {
    return YuzuRuntime::unavailable_message();
}

std::optional<ResolvedStandaloneEmulator> ensure_yuzu_runtime() {
    return YuzuRuntime::ensure();
}

YuzuUserProfile prepare_yuzu_user_profile(
    const SaveProfile& save_profile,
    bool force_opengl,
    bool force_vulkan,
    int vulkan_device,
    int resolution_scale) {
    return YuzuUserProfileService::prepare(
        save_profile, force_opengl, force_vulkan, vulkan_device, resolution_scale);
}

void configure_yuzu_archstreamer_controls(
    const YuzuUserProfile& profile,
    const std::vector<std::string>& sdl_guids) {
    YuzuControls::configure_archstreamer_controls(profile, sdl_guids);
}

std::vector<std::pair<std::string, std::string>> yuzu_launch_environment(const YuzuUserProfile& profile) {
    return YuzuLaunchEnv::launch_environment(profile);
}

std::filesystem::path default_ryujinx_runtime_root() {
    return RyujinxRuntime::runtime_root();
}

bool ryujinx_runtime_available() {
    return RyujinxRuntime::available();
}

std::string ryujinx_unavailable_message() {
    return RyujinxRuntime::unavailable_message();
}

std::optional<ResolvedStandaloneEmulator> ensure_ryujinx_runtime() {
    return RyujinxRuntime::ensure();
}

std::optional<ResolvedStandaloneEmulator> resolve_switch_runtime() {
    return SwitchRuntime::resolve();
}

std::string switch_runtime_unavailable_message() {
    return SwitchRuntime::unavailable_message();
}

RyujinxUserProfile prepare_ryujinx_user_profile(
    const SaveProfile& save_profile,
    bool enable_ldn_mitm,
    int resolution_scale,
    std::string_view profile_display_name) {
    return RyujinxUserProfileService::prepare(
        save_profile, enable_ldn_mitm, resolution_scale, profile_display_name);
}

void configure_ryujinx_archstreamer_controls(
    RyujinxUserProfile& profile,
    const std::vector<ArchStreamerSdlPad>& pads,
    const std::string& sdl_device_filter) {
    RyujinxControls::configure_archstreamer_controls(profile, pads, sdl_device_filter);
}

std::vector<std::pair<std::string, std::string>> ryujinx_launch_environment(
    const RyujinxUserProfile& profile) {
    return RyujinxLaunchEnv::launch_environment(profile);
}

std::string resolve_switch_profile_display_name(
    std::string_view save_username,
    const std::optional<ClientHello>& host_hello,
    const std::vector<ClientHello>& client_hellos) {
    if (host_hello.has_value() &&
        host_hello->username == save_username &&
        !host_hello->display_name.empty()) {
        return host_hello->display_name;
    }
    for (const auto& hello : client_hellos) {
        if (hello.username == save_username && !hello.display_name.empty()) {
            return hello.display_name;
        }
    }
    return std::string(save_username);
}

SwitchStandalonePrepResult prepare_switch_standalone(
    RetroArchLaunchConfig& launch_config,
    SwitchStandalonePrepInput input) {
    SwitchStandalonePrepResult result;
    result.resolved_pads = std::move(input.resolved_pads);

    if (result.resolved_pads.empty()) {
        result.resolved_pads = find_archstreamer_sdl_pads(
            input.players,
            input.ignore_controller,
            input.verbose,
            input.product_id_base);
    }

    if (input.graphics_api == GraphicsApiPreference::OpenGL) {
        result.force_opengl = true;
    } else if (input.graphics_api == GraphicsApiPreference::Vulkan) {
        if (input.virtualgl_capture) {
            std::cerr << "Warning: VirtualGL path cannot present Vulkan; using OpenGL.\n";
            result.force_opengl = true;
        } else {
            result.force_vulkan = true;
        }
    } else if (input.virtualgl_capture) {
        result.force_opengl = true;
    } else if (input.gamescope_capture) {
        result.force_vulkan = true;
    }

    const auto core_name = launch_config.core_path.filename().string();
    result.use_ryujinx =
        core_name.find("Ryujinx") != std::string::npos ||
        core_name.find("ryujinx") != std::string::npos;

    launch_config.quiet_stdio = !input.verbose;

    if (result.use_ryujinx) {
        result.enable_soft_keyboard = true;
        auto ryujinx_user = prepare_ryujinx_user_profile(
            input.save_profile,
            /*enable_ldn_mitm=*/true,
            input.resolution_scale,
            input.profile_display_name);
        const auto ryujinx_pads = resolve_exclusive_archstreamer_pads(
            input.players,
            input.verbose,
            input.product_id_base,
            result.resolved_pads);
        result.resolved_pads = ryujinx_pads.pads;
        configure_ryujinx_archstreamer_controls(
            ryujinx_user, ryujinx_pads.pads, ryujinx_pads.sdl_device_filter);
        result.ryujinx_profile = std::move(ryujinx_user);
        launch_config.standalone_args_before_content = {"--fullscreen"};
    } else {
        if (input.resolved_gpu != nullptr && input.resolved_gpu->has_value()) {
            result.yuzu_vulkan_device = yuzu_vulkan_device_index(**input.resolved_gpu);
        }
        auto yuzu_user = prepare_yuzu_user_profile(
            input.save_profile,
            result.force_opengl,
            result.force_vulkan,
            result.yuzu_vulkan_device,
            input.resolution_scale);
        std::vector<std::string> pad_guids;
        pad_guids.reserve(result.resolved_pads.size());
        for (const auto& pad : result.resolved_pads) {
            pad_guids.push_back(pad.guid);
        }
        configure_yuzu_archstreamer_controls(yuzu_user, pad_guids);
        result.yuzu_profile = std::move(yuzu_user);
        launch_config.standalone_args_before_content = {"-f", "-g"};
    }

    const auto synced = sync_switch_shared_saves_for_profile(input.save_profile);
    result.synced_title_count = synced.size();
    return result;
}

} // namespace archstreamer
