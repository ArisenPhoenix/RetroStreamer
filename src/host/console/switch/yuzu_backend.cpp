#include "host/console/switch/switch_backend.hpp"

#include "host/virtual/pad_plan.hpp"
#include "host/console/standalone_emulator.hpp"

namespace archstreamer {

SwitchBackendPrepResult YuzuBackend::prepare(
    RetroArchLaunchConfig& launch_config,
    SwitchBackendPrepContext ctx) {
    SwitchBackendPrepResult result;
    apply_common_prep(launch_config, ctx, result);

    if (ctx.resolved_gpu.has_value()) {
        result.yuzu_vulkan_device = ctx.resolved_gpu->yuzu_vulkan_index;
    }
    auto yuzu_user = prepare_yuzu_user_profile(
        ctx.save_profile,
        result.force_opengl,
        result.force_vulkan,
        result.yuzu_vulkan_device,
        ctx.resolution_scale);
    std::vector<std::string> pad_guids;
    pad_guids.reserve(result.resolved_pads.size());
    for (const auto& pad : result.resolved_pads) {
        pad_guids.push_back(pad.guid);
    }
    configure_yuzu_archstreamer_controls(yuzu_user, pad_guids);
    result.yuzu_profile = std::move(yuzu_user);
    launch_config.standalone_args_before_content = {"-f", "-g"};

    finish_prep_save_sync(ctx, result);
    return result;
}

void YuzuBackend::assign_launch_env_profile(
    EmulatorLaunchEnvRequest& env,
    SwitchBackendPrepResult& prep) const {
    if (prep.yuzu_profile.has_value()) {
        env.profile_env.merge_pairs(yuzu_launch_environment(*prep.yuzu_profile));
    }
    // Yuzu binds by GUID under the shared IGNORE blacklist (not exclusive EXCEPT).
    PadPlan plan;
    plan.mode = PadPlanMode::Shared;
    plan.pads = prep.resolved_pads;
    plan.ignore_devices = env.ignore_devices;
    env.plug.seat = std::move(plan);
}

} // namespace archstreamer
