#include "client/gstreamer_media_receiver.hpp"

#include <stdexcept>
#include <string_view>

namespace archstreamer {

std::uint16_t video_port_from_endpoint(const MediaEndpoint& endpoint) {
    constexpr std::string_view prefix = "rtp+h264://";
    if (endpoint.video_uri.rfind(prefix, 0) != 0) {
        throw std::runtime_error("unsupported video endpoint: " + endpoint.video_uri);
    }

    const auto colon = endpoint.video_uri.rfind(':');
    if (colon == std::string::npos || colon + 1 >= endpoint.video_uri.size()) {
        throw std::runtime_error("video endpoint is missing a port");
    }

    return static_cast<std::uint16_t>(std::stoul(endpoint.video_uri.substr(colon + 1)));
}

std::uint16_t audio_port_from_endpoint(const MediaEndpoint& endpoint) {
    constexpr std::string_view prefix = "rtp+opus://";
    if (endpoint.audio_uri.rfind(prefix, 0) != 0) {
        throw std::runtime_error("unsupported audio endpoint: " + endpoint.audio_uri);
    }

    const auto colon = endpoint.audio_uri.rfind(':');
    if (colon == std::string::npos || colon + 1 >= endpoint.audio_uri.size()) {
        throw std::runtime_error("audio endpoint is missing a port");
    }

    return static_cast<std::uint16_t>(std::stoul(endpoint.audio_uri.substr(colon + 1)));
}

void GStreamerMediaReceiver::connect(const MediaEndpoint& endpoint) {
    if (!endpoint.video_uri.empty()) {
        const auto port = video_port_from_endpoint(endpoint);
        video_process_.start({
            "gst-launch-1.0",
            "-q",
            "udpsrc",
            "port=" + std::to_string(port),
            "caps=application/x-rtp,media=video,encoding-name=H264,payload=96,clock-rate=90000",
            "!",
            "rtpjitterbuffer",
            "latency=40",
            "!",
            "rtph264depay",
            "!",
            "avdec_h264",
            "!",
            "videoconvert",
            "!",
            "autovideosink",
            "sync=false",
        });
    }

    if (!endpoint.audio_uri.empty()) {
        const auto port = audio_port_from_endpoint(endpoint);
        audio_process_.start({
            "gst-launch-1.0",
            "-q",
            "udpsrc",
            "port=" + std::to_string(port),
            "caps=application/x-rtp,media=audio,encoding-name=OPUS,payload=97,clock-rate=48000,encoding-params=2",
            "!",
            "rtpjitterbuffer",
            "latency=40",
            "!",
            "rtpopusdepay",
            "!",
            "opusdec",
            "!",
            "audioconvert",
            "!",
            "audioresample",
            "!",
            "autoaudiosink",
            "sync=false",
        });
    }
}

void GStreamerMediaReceiver::disconnect() {
    audio_process_.stop();
    video_process_.stop();
}

} // namespace archstreamer
