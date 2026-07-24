#pragma once

#ifdef _WIN32

#include "client/gstreamer_media_types.hpp"
#include "client/gstreamer_probe.hpp"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace archstreamer {

// Windows GStreamer pipeline choices for the media receivers.
class WindowsGStreamerMediaPlatform {
public:
    static const char* gst_launch_bin();

    static H264DecoderChoice choose_h264_decoder();

    static GstVideoSinkChoice choose_video_sink(bool prefer_d3d11);

    static std::vector<std::string> standalone_video_pipeline(
        std::uint16_t port,
        const H264DecoderChoice& decoder,
        const GstVideoSinkChoice& sink,
        bool sync);

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

#endif // _WIN32
