#include "client/video_window_geometry.hpp"

namespace archstreamer {

// d3d11videosink window placement across gst-launch restarts is not wired yet.
VideoWindowGeometry capture_video_window_geometry(int) {
    return {};
}

bool apply_video_window_geometry(
    int,
    const VideoWindowGeometry&,
    std::chrono::milliseconds) {
    return false;
}

} // namespace archstreamer
