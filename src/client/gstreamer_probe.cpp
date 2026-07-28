#include "client/gstreamer_probe.hpp"

#include "common/platform/process_utils.hpp"

#include <cstring>
#include <string>
#include <cstddef>

namespace archstreamer {
namespace {

#ifdef _WIN32
constexpr const char* kDevNull = "NUL";
#else
constexpr const char* kDevNull = "/dev/null";
#endif

int run_quiet(const std::string& command) {
    // Windows GUI builds: std::system() opens a console per probe on Join.
    return run_command_exit_code(command.c_str());
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
    // Wayland: prefer GTK (decorated, reliable on Bazzite). Avoid glimagesink as a
    // high-priority choice — it often opens a tiny centered window. Prefer X11 sinks
    // when DISPLAY is set so remoted-keyboard XQueryKeymap works with video focus.
    const bool wayland_session = [] {
        if (const char* wayland = std::getenv("WAYLAND_DISPLAY");
            wayland != nullptr && wayland[0] != '\0') {
            return true;
        }
        if (const char* session = std::getenv("XDG_SESSION_TYPE");
            session != nullptr && std::strcmp(session, "wayland") == 0) {
            return true;
        }
        return false;
    }();
    const bool have_x11 = [] {
        if (const char* display = std::getenv("DISPLAY");
            display != nullptr && display[0] != '\0') {
            return true;
        }
        return false;
    }();
    static constexpr const char* kWaylandWithX11[] = {
        // Prefer X11 sinks whenever DISPLAY exists (incl. XWayland on Bazzite):
        // - Closing the window EOSes gst-launch (gtksink often leaves audio running).
        // - X PutImage paints every buffer; gtksink can skip redraws on static GB
        //   screens until continuous animation starts (Pokemon title idle → bob).
        "ximagesink",
        "xvimagesink",
        "gtksink",
        "gtkglsink",
        "waylandsink",
        "glimagesink",
        "autovideosink",
    };
    static constexpr const char* kWaylandOnly[] = {
        "gtksink",
        "gtkglsink",
        "waylandsink",
        "glimagesink",
        "autovideosink",
    };
    static constexpr const char* kX11First[] = {
        "ximagesink",
        "xvimagesink",
        "gtksink",
        "gtkglsink",
        "glimagesink",
        "autovideosink",
    };
    const char* const* candidates = kX11First;
    std::size_t count = sizeof(kX11First) / sizeof(kX11First[0]);
    if (wayland_session) {
        if (have_x11) {
            candidates = kWaylandWithX11;
            count = sizeof(kWaylandWithX11) / sizeof(kWaylandWithX11[0]);
        } else {
            candidates = kWaylandOnly;
            count = sizeof(kWaylandOnly) / sizeof(kWaylandOnly[0]);
        }
    }
    for (std::size_t i = 0; i < count; ++i) {
        if (gst_video_sink_usable(candidates[i])) {
            return {candidates[i], kind_for_sink(candidates[i])};
        }
    }
#endif
    return {"autovideosink", GstVideoSinkKind::Other};
}

} // namespace archstreamer
