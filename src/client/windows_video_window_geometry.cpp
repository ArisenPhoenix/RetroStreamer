#include "client/video_window_geometry.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

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

int primary_display_height() {
    const int height = GetSystemMetrics(SM_CYSCREEN);
    return height > 0 ? height : 0;
}

} // namespace archstreamer
