#include "client/posix_gstreamer_media_platform.hpp"

#include "client/gstreamer_media_pipeline.hpp"
#include "client/gstreamer_probe.hpp"

#include <stdexcept>

namespace archstreamer {

const char* PosixGStreamerMediaPlatform::gst_launch_bin() {
    return "gst-launch-1.0";
}

H264DecoderChoice PosixGStreamerMediaPlatform::choose_h264_decoder() {
    if (!gst_inspect_available()) {
        throw std::runtime_error(
            "gst-inspect-1.0 not found on PATH. Install GStreamer tools and ensure they are on PATH.");
    }
    if (gst_element_available("avdec_h264")) {
        return {"avdec_h264", false};
    }
    if (gst_element_available("openh264dec")) {
        return {"openh264dec", false};
    }
    throw std::runtime_error(
        "no H.264 decoder found (need avdec_h264 or openh264dec). "
        "On Flatpak, install org.freedesktop.Platform.openh264 matching the runtime.");
}

GstVideoSinkChoice PosixGStreamerMediaPlatform::choose_video_sink(bool prefer_d3d11) {
    return choose_usable_video_sink(prefer_d3d11);
}

void PosixGStreamerMediaPlatform::append_video_branch(
    std::vector<std::string>& args,
    std::uint16_t port,
    const H264DecoderChoice& decoder,
    const GstVideoSinkChoice& sink,
    bool sync) {
    auto source = gst_h264_rtp_source_args(port);
    args.insert(args.end(), source.begin(), source.end());
    gst_append_h264parse_if_available(args);
    args.push_back(decoder.element);
    args.push_back("!");
    gst_append_progress_video_sink(args, sink, sync);
}

std::vector<std::string> PosixGStreamerMediaPlatform::standalone_video_pipeline(
    std::uint16_t port,
    const H264DecoderChoice& decoder,
    const GstVideoSinkChoice& sink,
    bool sync) {
    std::vector<std::string> args{gst_launch_bin()};
    append_video_branch(args, port, decoder, sink, sync);
    return args;
}

void PosixGStreamerMediaPlatform::configure_display_for_sink(
    const GstVideoSinkChoice& sink,
    std::vector<std::pair<std::string, std::string>>& environment,
    std::vector<std::string>& unset) {
    if (sink.kind != GstVideoSinkKind::X11) {
        return;
    }
    // Only strip Wayland when driving an X11 sink; otherwise the sink has no display.
    environment.push_back({"GDK_BACKEND", "x11"});
    environment.push_back({"GST_GL_WINDOW", "x11"});
    environment.push_back({"GST_GL_PLATFORM", "glx"});
    unset.push_back("WAYLAND_DISPLAY");
    unset.push_back("WAYLAND_SOCKET");
}

} // namespace archstreamer
