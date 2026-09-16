#include "host/video/capture_platform.hpp"

#include <iostream>

namespace archstreamer {

bool platform_supports_gamescope_capture() {
    return false;
}

bool platform_supports_virtualgl_capture() {
    return false;
}

PresentTarget resolve_present_target(CaptureResolveInput& input) {
    PresentTarget present;
    present.display = input.capture_display;
    if (!input.stream_video) {
        return present;
    }
    present.backend = input.preferred_backend;
    if (present.backend == VirtualDisplayBackend::None) {
        present.backend = VirtualDisplayBackend::Desktop;
    }
    return present;
}

void apply_standalone_capture_prefix(
    std::vector<std::string>& /*command_prefix*/,
    const PresentTarget& present,
    const std::string& /*video_resolution*/,
    const std::string& /*gamescope_vk_device*/) {
    if (present.captures()) {
        std::cout
            << "Yuzu: Windows desktop capture (d3d11screencapturesrc / WASAPI loopback)\n";
    }
}

void apply_retroarch_vgl_prefix(
    std::vector<std::string>& /*command_prefix*/,
    const PresentTarget& /*present*/,
    const std::optional<GpuTarget>& /*resolved_gpu*/) {
    // VirtualGL is Linux-only.
}

void start_deferred_gamescope_video_if_needed(
    MediaServer* /*media_server*/,
    const std::string& /*video_resolution*/,
    std::vector<MediaClientStream>& /*media_streams*/,
    int /*owner_pid*/) {
    // Gamescope PipeWire path is Linux-only.
}

} // namespace archstreamer
