#include "host/contracts/launch.hpp"

namespace archstreamer {

void cleanup_x11_capture_runtime_dir() {}

ProcessEnvironment audio_launch_environment(
    bool /*stream_media*/,
    bool /*stream_audio*/,
    bool /*host_plays_locally*/,
    const AudioTap& /*audio*/) {
    return {};
}

ProcessEnvironment capture_launch_environment(const PresentTarget& /*present*/) {
    return {};
}

} // namespace archstreamer
