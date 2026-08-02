#include "host/switch/switch_backend.hpp"

#include "host/standalone_emulator.hpp"
#include "host/virtual_joypad_resolve.hpp"

namespace archstreamer {

SwitchBackendPrepResult RyujinxBackend::prepare(
    RetroArchLaunchConfig& launch_config,
    SwitchBackendPrepContext ctx) {
    SwitchBackendPrepResult result;
    apply_common_prep(launch_config, ctx, result);

    auto ryujinx_user = prepare_ryujinx_user_profile(
        ctx.save_profile,
        /*enable_ldn_mitm=*/true,
        ctx.resolution_scale,
        ctx.profile_display_name);
    const auto ryujinx_pads = resolve_exclusive_archstreamer_pads(
        ctx.players,
        ctx.verbose,
        ctx.product_id_base,
        result.resolved_pads);
    result.resolved_pads = ryujinx_pads.pads;
    configure_ryujinx_archstreamer_controls(
        ryujinx_user, ryujinx_pads.pads, ryujinx_pads.sdl_device_filter);
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
}

} // namespace archstreamer
