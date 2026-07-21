#include "client/gstreamer_media_receiver.hpp"

#include "common/addresses.hpp"

namespace archstreamer {

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
            "sync=true",
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
            "latency=100",
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
            "sync=true",
        });
    }
}

void GStreamerMediaReceiver::disconnect() {
    audio_process_.stop();
    video_process_.stop();
}

} // namespace archstreamer
