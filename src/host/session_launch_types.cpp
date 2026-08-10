#include "host/session_launch_types.hpp"

#include <chrono>
#include <sstream>
#include <thread>
#include <utility>

namespace archstreamer {

std::string SessionDevicePlan::capture_info() const {
    std::ostringstream o;
    o << "Capture: " << (capture_fullscreen ? "fullscreen" : "windowed") << video_resolution
      << " on display " << capture_display
      << (use_virtual_capture ? " (virtual)" : " (host)") << '\n';
    return o.str();
}

void apply_capture_to_session_device_plan(
    SessionDevicePlan& devices,
    const HostAppConfig& config,
    const CapturePlan& capture,
    std::optional<GpuDevice> resolved_gpu) {
    devices.resolved_gpu = std::move(resolved_gpu);
    devices.use_virtual_capture = capture.use_virtual_capture;
    devices.capture_fullscreen = capture.capture_fullscreen;
    devices.capture_display = capture.capture_display;
    devices.display_backend = capture.display_backend;
    devices.video_resolution = config.video_resolution;
}

void plug_session_gamepads(VirtualGamepadBus& gamepads, RetroArchPort players) {
    for (RetroArchPort port = 0; port < players; ++port) {
        gamepads.plug(port);
    }
}

void wait_for_session_input_enumeration() {
    std::this_thread::sleep_for(std::chrono::milliseconds(750));
}

} // namespace archstreamer
