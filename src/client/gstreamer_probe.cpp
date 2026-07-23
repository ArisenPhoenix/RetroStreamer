#include "client/gstreamer_probe.hpp"

#include <cstdlib>
#include <cstring>
#include <string>

namespace archstreamer {
namespace {

#ifdef _WIN32
constexpr const char* kDevNull = "NUL";
#else
constexpr const char* kDevNull = "/dev/null";
#endif

int run_quiet(const std::string& command) {
    return std::system(command.c_str());
}

GstVideoSinkKind kind_for_sink(const char* element) {
    if (std::strcmp(element, "ximagesink") == 0 || std::strcmp(element, "xvimagesink") == 0) {
        return GstVideoSinkKind::X11;
    }
    if (std::strcmp(element, "waylandsink") == 0) {
        return GstVideoSinkKind::Wayland;
    }
    if (std::strcmp(element, "d3d11videosink") == 0) {
        return GstVideoSinkKind::D3D11;
    }
    return GstVideoSinkKind::Other;
}

} // namespace

bool gst_inspect_available() {
#ifdef _WIN32
    return run_quiet(std::string("gst-inspect-1.0 --version >") + kDevNull + " 2>&1") == 0;
#else
    return run_quiet(std::string("gst-inspect-1.0 --version >") + kDevNull + " 2>&1") == 0;
#endif
}

bool gst_element_available(const char* element) {
    if (element == nullptr || element[0] == '\0') {
        return false;
    }
    return run_quiet(
               std::string("gst-inspect-1.0 ") + element + " >" + kDevNull + " 2>&1") == 0;
}

bool gst_video_sink_usable(const char* element) {
    if (!gst_element_available(element)) {
        return false;
    }
    // One frame is enough to prove the sink can initialise (catches missing Xv, etc.).
#ifdef _WIN32
    const auto command = std::string("gst-launch-1.0 -q videotestsrc num-buffers=1 ! videoconvert ! ") +
        element + " >" + kDevNull + " 2>&1";
#else
    // Bound the probe so a wedged sink cannot hang connect forever.
    const auto command = std::string("timeout 5 gst-launch-1.0 -q videotestsrc num-buffers=1 ! videoconvert ! ") +
        element + " >" + kDevNull + " 2>&1";
#endif
    return run_quiet(command) == 0;
}

GstVideoSinkChoice choose_usable_video_sink(bool prefer_d3d11) {
#ifdef _WIN32
    const char* candidates[3] = {};
    int count = 0;
    if (prefer_d3d11) {
        candidates[count++] = "d3d11videosink";
    }
    candidates[count++] = "d3d11videosink";
    candidates[count++] = "autovideosink";
    for (int i = 0; i < count; ++i) {
        // Skip duplicate d3d11 entry when prefer_d3d11 is set.
        if (i > 0 && std::strcmp(candidates[i], candidates[i - 1]) == 0) {
            continue;
        }
        if (gst_video_sink_usable(candidates[i])) {
            return {candidates[i], kind_for_sink(candidates[i])};
        }
    }
#else
    (void)prefer_d3d11;
    // Trial order only — first sink that actually opens wins (ximagesink before
    // xvimagesink so QXL/SPICE VMs are not stuck on a registered-but-broken Xv sink).
    static constexpr const char* kCandidates[] = {
        "ximagesink",
        "glimagesink",
        "waylandsink",
        "xvimagesink",
        "autovideosink",
    };
    for (const char* candidate : kCandidates) {
        if (gst_video_sink_usable(candidate)) {
            return {candidate, kind_for_sink(candidate)};
        }
    }
#endif
    return {"autovideosink", GstVideoSinkKind::Other};
}

} // namespace archstreamer
