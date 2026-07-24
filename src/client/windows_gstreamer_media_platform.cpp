#include "client/windows_gstreamer_media_platform.hpp"

#ifdef _WIN32

#include "client/gstreamer_media_pipeline.hpp"
#include "client/gstreamer_probe.hpp"

#include <stdexcept>

namespace archstreamer {

const char* WindowsGStreamerMediaPlatform::gst_launch_bin() {
    // Must include .exe: CreateProcess treats "gst-launch-1.0" as extension ".0".
    return "gst-launch-1.0.exe";
}

H264DecoderChoice WindowsGStreamerMediaPlatform::choose_h264_decoder() {
    if (!gst_inspect_available()) {
        throw std::runtime_error(
            "gst-inspect-1.0 not found on PATH. Install the GStreamer MSVC runtime "
            "(https://gstreamer.freedesktop.org/download/) and add its bin directory to PATH, "
            "then restart ArchStreamer.");
    }
    if (gst_element_available("d3d11h264dec")) {
        return {"d3d11h264dec", true};
    }
    if (gst_element_available("mfh264dec")) {
        return {"mfh264dec", false};
    }
    if (gst_element_available("avdec_h264")) {
        return {"avdec_h264", false};
    }
    if (gst_element_available("openh264dec")) {
        return {"openh264dec", false};
    }
    throw std::runtime_error(
        "no H.264 decoder found (need d3d11h264dec, mfh264dec, avdec_h264, or openh264dec). "
        "Install the full GStreamer MSVC runtime (MSVC 64-bit), not just a minimal package, "
        "and ensure its bin directory is on PATH.");
}

GstVideoSinkChoice WindowsGStreamerMediaPlatform::choose_video_sink(bool prefer_d3d11) {
    return choose_usable_video_sink(prefer_d3d11);
}

void WindowsGStreamerMediaPlatform::append_video_branch(
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

    if (decoder.d3d11_zero_copy && sink.kind == GstVideoSinkKind::D3D11) {
        // Keep frames in D3D11 memory for the Windows HW path.
        args.push_back(sink.element);
        args.push_back(sync ? "sync=true" : "sync=false");
        return;
    }

    if (decoder.d3d11_zero_copy) {
        args.push_back("d3d11download");
        args.push_back("!");
    }

    gst_append_progress_video_sink(args, sink, sync);
}

std::vector<std::string> WindowsGStreamerMediaPlatform::standalone_video_pipeline(
    std::uint16_t port,
    const H264DecoderChoice& decoder,
    const GstVideoSinkChoice& sink,
    bool sync) {
    std::vector<std::string> args{gst_launch_bin()};
    append_video_branch(args, port, decoder, sink, sync);
    return args;
}

void WindowsGStreamerMediaPlatform::configure_display_for_sink(
    const GstVideoSinkChoice&,
    std::vector<std::pair<std::string, std::string>>&,
    std::vector<std::string>&) {
    // Windows sinks do not need X11/Wayland display remapping.
}

} // namespace archstreamer

#endif // _WIN32
