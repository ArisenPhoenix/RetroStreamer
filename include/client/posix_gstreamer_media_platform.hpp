#pragma once

#include "client/gstreamer_media_types.hpp"
#include "client/gstreamer_probe.hpp"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace archstreamer {

// Linux / POSIX GStreamer pipeline choices for the media receivers.
class PosixGStreamerMediaPlatform {
public:
    static const char* gst_launch_bin();

    static H264DecoderChoice choose_h264_decoder();

    static GstVideoSinkChoice choose_video_sink(bool prefer_d3d11);

    // Standalone gst-launch argv for the legacy dual-process video receiver.
    static std::vector<std::string> standalone_video_pipeline(
        std::uint16_t port,
        const H264DecoderChoice& decoder,
        const GstVideoSinkChoice& sink,
        bool sync);

    // Append a video branch (no gst-launch bin) for the synced A/V process.
    static void append_video_branch(
        std::vector<std::string>& args,
        std::uint16_t port,
        const H264DecoderChoice& decoder,
        const GstVideoSinkChoice& sink,
        bool sync);

    static void configure_display_for_sink(
        const GstVideoSinkChoice& sink,
        std::vector<std::pair<std::string, std::string>>& environment,
        std::vector<std::string>& unset);
};

} // namespace archstreamer
