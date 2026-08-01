#pragma once

#include <chrono>
#include <cstdint>

namespace archstreamer {

/** Last known placement of the gst-launch video sink window. */
struct VideoWindowGeometry {
    bool valid = false;
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    /** User expanded the window (maximized / “fill work area”). */
    bool maximized = false;
    bool fullscreen = false;
};

/**
 * Read position / size / expand state for the top-level window owned by pid.
 * Empty when the process has no mapped video window yet.
 */
VideoWindowGeometry capture_video_window_geometry(int pid);

/**
 * Wait for pid's video window, then apply geometry. Best-effort: silent no-op
 * when the display/server is unavailable or the window never appears.
 */
bool apply_video_window_geometry(
    int pid,
    const VideoWindowGeometry& geometry,
    std::chrono::milliseconds timeout = std::chrono::milliseconds(2500));

/** Primary screen height in pixels (0 if unavailable). Used for Auto stream size. */
int primary_display_height();

} // namespace archstreamer
