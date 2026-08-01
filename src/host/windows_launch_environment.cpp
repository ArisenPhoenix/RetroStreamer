#include "host/launch_environment.hpp"

namespace archstreamer {

void cleanup_x11_capture_runtime_dir() {}

ProcessEnvironment audio_launch_environment(
    bool /*stream_media*/,
    bool /*stream_audio*/,
    bool /*host_plays_locally*/,
    const std::string& /*audio_source*/) {
    return {};
}

ProcessEnvironment capture_launch_environment(
    bool /*use_virtual_capture*/,
    bool /*gamescope_capture*/,
    bool /*virtualgl_capture*/,
    const std::string& /*capture_display*/) {
    return {};
}

} // namespace archstreamer
