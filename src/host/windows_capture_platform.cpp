#include "host/capture_platform.hpp"

#include <iostream>
#include "host/gpu_select.hpp"

namespace archstreamer {

bool platform_supports_gamescope_capture() {
    return false;
}

bool platform_supports_virtualgl_capture() {
    return false;
}

CapturePlan resolve_capture_plan(
    HostAppConfig& config,
    const RetroArchLaunchConfig& /*launch_config*/) {
    CapturePlan plan;
    plan.use_virtual_capture = config.video;
    plan.capture_fullscreen = config.video;
    plan.capture_display = config.virtual_display;
    plan.display_backend = config.display_backend;
    // Windows streams via d3d11screencapturesrc / desktop capture — no Xvfb/gamescope.
    plan.gamescope_capture = false;
    plan.virtualgl_capture = false;
    return plan;
}

void normalize_audio_backend_for_platform(HostAppConfig& config) {
    if (config.audio_backend == AudioCaptureBackend::Pulse) {
        config.audio_backend = AudioCaptureBackend::Wasapi;
    }
}

void apply_standalone_capture_prefix(
    RetroArchLaunchConfig& launch_config,
    const CapturePlan& capture,
    const HostAppConfig& /*config*/,
    const std::string& /*gamescope_vk_device*/) {
    if (launch_config.standalone && capture.use_virtual_capture) {
        std::cout
            << "Yuzu: Windows desktop capture (d3d11screencapturesrc / WASAPI loopback)\n";
    }
}

void apply_retroarch_vgl_prefix(
    RetroArchLaunchConfig& /*launch_config*/,
    const CapturePlan& /*capture*/,
    const std::optional<GpuDevice>& /*resolved_gpu*/) {
    // VirtualGL is Linux-only.
}

void start_deferred_gamescope_video_if_needed(
    MediaServer* /*media_server*/,
    const HostAppConfig& /*config*/,
    std::vector<MediaClientStream>& /*media_streams*/) {
    // Gamescope PipeWire path is Linux-only.
}

} // namespace archstreamer
