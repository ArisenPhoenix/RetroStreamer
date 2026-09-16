#include "host/session/backend_state.hpp"

#include "host/console/standalone_emulator.hpp"

#include <stdexcept>
#include <utility>

namespace archstreamer {

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

SessionBackendPrepareContext make_session_backend_prepare_context(
    const HostAppConfig& config,
    HostLaunchPlan& launch_plan,
    SessionLaunchAssets& assets,
    SessionDevicePlan& devices,
    SessionCaptureDevices& capture,
    SessionBackendState& backends,
    EmulatorLaunchEnvRequest& launch_env_request,
    SessionParticipantContext participants) {
    return SessionBackendPrepareContext{
        config,
        SessionUserContext{
            assets.save_profile,
            std::move(participants),
        },
        SessionGameContext{
            launch_plan,
            assets.launch_config,
            assets.content,
        },
        SessionVideoContext{
            capture,
            config.media.video.resolution,
            config.emulator.resolution.retroarch_scale,
            config.emulator.resolution.switch_scale,
        },
        SessionInputContext{
            devices,
        },
        backends,
        launch_env_request,
    };
}

RetroArchOverrideParams build_session_retroarch_override(
    const SessionBackendPrepareContext& backend,
    const SessionRetroArchOverrideOptions& options) {
    const auto& config = backend.config;
    const auto& user = backend.user;
    const auto& game = backend.game;
    const auto& video = backend.video;
    const auto& input = backend.input;
    const auto& backends = backend.backends;

    RetroArchOverrideParams override_params;
    override_params.first_virtual_joypad_index = input.devices.input.virtual_joypad_index;
    override_params.identities = &game.launch_plan.virtual_identities;
    override_params.joypad_driver = config.pads.retroarch_joypad_driver;
    override_params.players = game.launch_plan.players;
    override_params.save_profile = &user.save_profile;
    override_params.realtime_pacing = config.media.audio.enabled || config.media.video.enabled;
    override_params.capture_fullscreen = video.devices.present.captures();
    override_params.capture_resolution = std::string(video.video_resolution);
    override_params.vulkan_gpu_index =
        (!video.devices.present.captures() && video.devices.resolved_gpu.has_value())
            ? video.devices.resolved_gpu->vulkan_index
            : -1;
    override_params.system_key = backends.system_key;
    override_params.core_path = game.launch_config.core_path;
    override_params.resolution_scale = video.retroarch_scale;
    override_params.slot_index = options.slot_index;
    override_params.network_cmd_port = options.network_cmd_port;
    override_params.display_layout = options.display_layout;
    return override_params;
}

} // namespace archstreamer
