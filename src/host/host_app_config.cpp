#include "host/host_app_config.hpp"

#include "common/participant_role.hpp"
#include "host/session_lobby.hpp"

#include <vector>

namespace archstreamer {
namespace {

const char* graphics_api_name(GraphicsApiPreference api) {
    switch (api) {
    case GraphicsApiPreference::OpenGL:
        return "opengl";
    case GraphicsApiPreference::Vulkan:
        return "vulkan";
    case GraphicsApiPreference::Auto:
    default:
        return "auto";
    }
}

} // namespace

HostMediaPlanConfig media_plan_config_for(const HostAppConfig& config) {
    return HostMediaPlanConfig{
        config.video,
        config.audio,
        config.video_destination,
        config.video_destination_explicit,
        config.video_port,
        config.audio_port,
    };
}

std::vector<std::string> host_app_config_to_argv(const HostAppConfig& config) {
    std::vector<std::string> args;
    args.push_back("--rom-root");
    args.push_back(config.rom_root.string());
    if (!config.meta_root.empty()) {
        args.push_back("--meta-root");
        args.push_back(config.meta_root.string());
    }
    if (!config.art_root.empty()) {
        args.push_back("--art-root");
        args.push_back(config.art_root.string());
    }
    if (config.control_port.has_value()) {
        args.push_back("--control-port");
        args.push_back(std::to_string(*config.control_port));
    }
    if (config.input_port.has_value()) {
        args.push_back("--input-port");
        args.push_back(std::to_string(*config.input_port));
    }
    args.push_back("--clients");
    args.push_back(std::to_string(config.clients));
    args.push_back("--session-timeout");
    args.push_back(std::to_string(config.session_timeout_seconds));
    args.push_back("--client-timeout");
    args.push_back(std::to_string(config.client_timeout_seconds));
    args.push_back("--player-reconnect-timeout");
    args.push_back(std::to_string(config.player_reconnect_timeout_seconds));
    args.push_back("--host-role");
    args.push_back(participant_role_name(config.host_role));
    args.push_back("--mode");
    args.push_back(session_mode_name(config.session_mode));
    args.push_back("--players");
    args.push_back(std::to_string(config.players));
    args.push_back("--gpu");
    args.push_back(config.encode_gpu);
    if (config.separate_render_gpu) {
        args.push_back("--separate-render-gpu");
        args.push_back("--render-gpu");
        args.push_back(config.render_gpu);
    }
    args.push_back("--renderer");
    args.push_back(graphics_api_name(config.graphics_api));
    args.push_back("--switch-resolution");
    args.push_back(std::to_string(config.resolution.switch_scale));
    args.push_back("--retroarch-resolution");
    args.push_back(std::to_string(config.resolution.retroarch_scale));

    if (config.verbose) {
        args.push_back("--verbose");
    }
    if (config.list) {
        args.push_back("--list");
    }
    if (config.dry_run) {
        args.push_back("--dry-run");
    }
    if (config.pulse_input) {
        args.push_back("--pulse-input");
    }

    if (config.bridge_controller_index.has_value()) {
        args.push_back("--bridge-controller");
        args.push_back(std::to_string(*config.bridge_controller_index));
    }
    if (config.virtual_joypad_index.has_value()) {
        args.push_back("--virtual-joypad-index");
        args.push_back(std::to_string(*config.virtual_joypad_index));
    }
    if (config.ignore_controller.has_value()) {
        args.push_back("--ignore-controller");
        args.push_back(*config.ignore_controller);
    }
    if (!config.retroarch_joypad_driver.empty()) {
        args.push_back("--retroarch-joypad-driver");
        args.push_back(config.retroarch_joypad_driver);
    }
    if (!config.save_root.empty()) {
        args.push_back("--save-root");
        args.push_back(config.save_root.string());
    }
    if (!config.username.empty()) {
        args.push_back("--username");
        args.push_back(config.username);
    }

    if (config.video) {
        args.push_back("--video");
        args.push_back("--video-port");
        args.push_back(std::to_string(config.video_port));
        if (config.video_destination_explicit) {
            args.push_back("--video-dest");
            args.push_back(config.video_destination);
        }
        args.push_back("--virtual-display");
        args.push_back(config.virtual_display);
        args.push_back("--video-resolution");
        args.push_back(config.video_resolution);
        if (config.display_backend != VirtualDisplayBackend::None) {
            args.push_back("--display-backend");
            switch (config.display_backend) {
            case VirtualDisplayBackend::Xvfb:
                args.push_back("xvfb");
                break;
            case VirtualDisplayBackend::Xephyr:
                args.push_back("xephyr");
                break;
            case VirtualDisplayBackend::VirtualGL:
                args.push_back("virtualgl");
                break;
            case VirtualDisplayBackend::Gamescope:
                args.push_back("gamescope");
                break;
            case VirtualDisplayBackend::None:
                break;
            }
        }
    } else {
        args.push_back("--no-video");
    }

    if (config.audio) {
        args.push_back("--audio");
        args.push_back("--audio-port");
        args.push_back(std::to_string(config.audio_port));
        if (!config.audio_source.empty()) {
            args.push_back("--audio-source");
            args.push_back(config.audio_source);
        }
        args.push_back("--audio-backend");
        args.push_back(
            config.audio_backend == AudioCaptureBackend::PipeWire ? "pipewire" : "pulse");
    } else {
        args.push_back("--no-audio");
    }

    if (config.selector.has_value()) {
        args.push_back(*config.selector);
    }
    return args;
}

} // namespace archstreamer
