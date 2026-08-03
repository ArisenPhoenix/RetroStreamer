#include "host/switch/switch_backend.hpp"

#include "host/pad_plan.hpp"
#include "host/standalone_emulator.hpp"
#include "host/switch/ldn_net_isolation.hpp"

namespace archstreamer {

SwitchBackendPrepResult RyujinxBackend::prepare(
    RetroArchLaunchConfig& launch_config,
    SwitchBackendPrepContext ctx) {
    SwitchBackendPrepResult result;
    apply_common_prep(launch_config, ctx, result);

#if !defined(_WIN32)
    // Dual same-host LDN: isolate each Ryujinx in a firejail netns on asldnbr0.
    auto ldn_prefix = ldn_firejail_command_prefix(ctx.slot_index);
    if (!ldn_prefix.empty()) {
        launch_config.network_namespace_prefix = std::move(ldn_prefix);
    }
    const auto lan_iface = ldn_guest_interface_name();
#else
    const std::string lan_iface;
#endif

    auto ryujinx_user = prepare_ryujinx_user_profile(
        ctx.save_profile,
        /*enable_ldn_mitm=*/true,
        ctx.resolution_scale,
        ctx.profile_display_name,
        lan_iface);
    auto pad_plan = resolve_exclusive_pad_plan(
        ctx.players,
        ctx.verbose,
        ctx.product_id_base,
        result.resolved_pads);
    if (!pad_plan.exclusive()) {
        pad_plan.ignore_devices = ctx.ignore_controller;
    }
    result.resolved_pads = pad_plan.pads;
    result.pad_plan = pad_plan;
    configure_ryujinx_archstreamer_controls(
        ryujinx_user, pad_plan.pads, pad_plan.exclusive_filter);
    result.ryujinx_profile = std::move(ryujinx_user);
    launch_config.standalone_args_before_content = {"--fullscreen"};

    finish_prep_save_sync(ctx, result);
    return result;
}

void RyujinxBackend::assign_launch_env_profile(
    EmulatorLaunchEnvRequest& env,
    SwitchBackendPrepResult& prep) const {
    env.ryujinx_profile = std::move(prep.ryujinx_profile);
    env.yuzu_profile.reset();
    if (prep.pad_plan.has_value()) {
        env.pad_plan = std::move(prep.pad_plan);
    }
}

} // namespace archstreamer
