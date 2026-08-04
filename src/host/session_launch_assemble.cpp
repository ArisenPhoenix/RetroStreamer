#include "host/session_launch_assemble.hpp"

#include "host/gpu_select.hpp"
#include "host/retroarch_config_writer.hpp"
#include "host/switch_save_share.hpp"

#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>

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
    std::optional<int> slot_index,
    const SwitchBackend* backend,
    std::string_view content_stem,
    std::string_view title_id) {
    const auto synced = backend != nullptr
        ? backend->post_exit_sync(profile, content_stem, title_id)
        : (!content_stem.empty()
            ? std::vector<std::string>{sync_catalog_switch_save_after_exit(
                  profile, content_stem, title_id)}
            : sync_switch_shared_saves_for_profile(profile));
    if (synced.empty() || (synced.size() == 1 && synced.front().empty())) {
        return;
    }
    if (slot_index.has_value()) {
        std::cout
            << "session slot " << *slot_index << ": post-exit Switch save sync ("
            << synced.size() << " leaf(s))\n";
    } else {
        std::cout << "Post-exit Switch save sync: " << synced.size() << " leaf(s)\n";
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
        params.network_cmd_port,
        params.display_layout);
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

DisplayLayoutPreference resolve_display_layout_preference(
    const std::optional<ClientHello>& host_hello,
    const std::vector<ClientHello>& client_hellos) {
    DisplayLayoutPreference first_explicit = DisplayLayoutPreference::Auto;
    bool saw_portrait = false;
    auto consider = [&](const ClientHello& hello) {
        if (!hello.wants_video) {
            return;
        }
        if (hello.display_layout == DisplayLayoutPreference::Portrait) {
            saw_portrait = true;
        } else if (
            hello.display_layout != DisplayLayoutPreference::Auto &&
            first_explicit == DisplayLayoutPreference::Auto) {
            first_explicit = hello.display_layout;
        }
    };
    if (host_hello.has_value()) {
        consider(*host_hello);
    }
    for (const auto& hello : client_hellos) {
        consider(hello);
    }
    if (saw_portrait) {
        return DisplayLayoutPreference::Portrait;
    }
    return first_explicit;
}

} // namespace archstreamer
