#pragma once

namespace archstreamer {

bool gst_inspect_available();
bool gst_element_available(const char* element);

/** True when a short videotestsrc pipeline can open this sink (plugin present is not enough). */
bool gst_video_sink_usable(const char* element);

enum class GstVideoSinkKind { X11, Wayland, D3D11, Other };

struct GstVideoSinkChoice {
    const char* element = "autovideosink";
    GstVideoSinkKind kind = GstVideoSinkKind::Other;
};

/**
 * Pick the first video sink that is installed and can open on this display.
 * Order is only a trial sequence; selection is based on a live open probe.
 */
GstVideoSinkChoice choose_usable_video_sink(bool prefer_d3d11);

} // namespace archstreamer
