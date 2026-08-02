#include "host/session_launch_assemble.hpp"

#include "host/gpu_select.hpp"
#include "host/retroarch_config_writer.hpp"
#include "host/switch_save_share.hpp"

#include <iostream>
#include <stdexcept>

namespace archstreamer {

void apply_capture_and_launch_environment(
    RetroArchLaunchConfig& launch_config,
    const CapturePlan& capture,
    const HostAppConfig& config,
    const std::string& gamescope_vk_device,
    const std::optional<GpuDevice>& resolved_gpu,
    const EmulatorLaunchEnvRequest& env_request) {
    if (launch_config.standalone) {
        apply_standalone_capture_prefix(
            launch_config, capture, config, gamescope_vk_device);
    } else {
        apply_retroarch_vgl_prefix(launch_config, capture, resolved_gpu);
    }
    apply_launch_env_to_config(launch_config, env_request);
}

void sync_and_log_post_exit_switch_saves(
    const SaveProfile& profile,
    std::optional<int> slot_index) {
    const auto synced = sync_switch_shared_saves_for_profile(profile);
    if (synced.empty()) {
        return;
    }
    if (slot_index.has_value()) {
        std::cout
            << "session slot " << *slot_index << ": post-exit Switch save sync ("
            << synced.size() << " title(s))\n";
    } else {
        std::cout << "Post-exit Switch save sync: " << synced.size() << " title(s)\n";
    }
}

void rewrite_retroarch_config_arg(
    RetroArchLaunchConfig& launch_config,
    const std::filesystem::path& runtime_override) {
    for (std::size_t i = 0; i + 1 < launch_config.extra_args.size(); ++i) {
        if (launch_config.extra_args[i] == "-c") {
            launch_config.extra_args[i + 1] = runtime_override.string();
            return;
        }
    }
    launch_config.extra_args.push_back("-c");
    launch_config.extra_args.push_back(runtime_override.string());
}

void apply_launch_env_to_config(
    RetroArchLaunchConfig& launch_config,
    const EmulatorLaunchEnvRequest& request) {
    const auto launch_env = build_emulator_launch_environment(request);
    launch_config.environment = launch_env.entries;
    launch_config.unset_environment = launch_env.unset;
}

std::filesystem::path apply_retroarch_override(
    RetroArchLaunchConfig& launch_config,
    const RetroArchOverrideParams& params) {
    if (params.identities == nullptr || params.save_profile == nullptr) {
        throw std::invalid_argument(
            "RetroArchOverrideParams requires identities and save_profile");
    }
    const auto runtime_override = write_retroarch_input_override(
        params.first_virtual_joypad_index,
        *params.identities,
        params.joypad_driver,
        params.players,
        *params.save_profile,
        params.realtime_pacing,
        params.capture_fullscreen,
        params.capture_resolution,
        params.vulkan_gpu_index,
        params.system_key,
        params.core_path,
        params.resolution_scale,
        params.slot_index,
        params.network_cmd_port);
    rewrite_retroarch_config_arg(launch_config, runtime_override);
    return runtime_override;
}

std::filesystem::path apply_retroarch_override_and_env(
    RetroArchLaunchConfig& launch_config,
    const RetroArchOverrideParams& params,
    const EmulatorLaunchEnvRequest& env_request) {
    const auto runtime_override = apply_retroarch_override(launch_config, params);
    apply_launch_env_to_config(launch_config, env_request);
    return runtime_override;
}

} // namespace archstreamer
