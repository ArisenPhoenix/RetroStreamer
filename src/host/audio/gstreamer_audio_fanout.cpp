#include "host/audio/gstreamer_audio_fanout.hpp"

#include "common/addresses.hpp"
#include "common/platform/process_utils.hpp"

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace archstreamer {
namespace {

std::string multiudp_clients_arg(
    const std::vector<std::pair<std::string, std::uint16_t>>& clients) {
    std::string joined;
    for (const auto& [host, port] : clients) {
        if (!joined.empty()) {
            joined.push_back(',');
        }
        joined += host;
        joined.push_back(':');
        joined += std::to_string(port);
    }
    return joined;
}

bool gst_element_available(const char* element) {
    if (element == nullptr || element[0] == '\0') {
        return false;
    }
    const auto output = read_command_output(
        (std::string("gst-inspect-1.0 ") + element + " 2>/dev/null").c_str());
    return !output.empty();
}

} // namespace

GStreamerAudioFanout::~GStreamerAudioFanout() {
    stop();
}

std::vector<MediaClientStream> GStreamerAudioFanout::start(
    AudioCaptureBackend backend,
    const std::string& source,
    const std::vector<MediaStreamRequest>& destinations) {
    if (!destinations_.empty() || process_.running()) {
        throw std::runtime_error("audio fanout is already running");
    }

    backend_ = backend;
    source_ = source;
    auto streams = std::vector<MediaClientStream>{};
    streams.reserve(destinations.size());
    for (const auto& destination : destinations) {
        destinations_.push_back(Destination{
            destination.client_id,
            destination.destination_host,
            destination.port,
        });
        streams.push_back(MediaClientStream{
            destination.client_id,
            destination.destination_host,
            MediaEndpoint{"", rtp_opus_uri(destination.destination_host, destination.port)},
        });
    }
    if (!destinations_.empty()) {
        restart_pipeline();
    }
    return streams;
}

MediaClientStream GStreamerAudioFanout::add(
    AudioCaptureBackend backend,
    const std::string& source,
    const MediaStreamRequest& destination) {
    backend_ = backend;
    source_ = source;
    stop_client(destination.client_id);

    destinations_.push_back(Destination{
        destination.client_id,
        destination.destination_host,
        destination.port,
    });
    restart_pipeline();

    return MediaClientStream{
        destination.client_id,
        destination.destination_host,
        MediaEndpoint{"", rtp_opus_uri(destination.destination_host, destination.port)},
    };
}

void GStreamerAudioFanout::stop() {
    process_.stop();
    destinations_.clear();
}

void GStreamerAudioFanout::stop_client(ClientId client_id) {
    const auto before = destinations_.size();
    destinations_.erase(
        std::remove_if(
            destinations_.begin(),
            destinations_.end(),
            [client_id](const Destination& destination) {
                return destination.client_id == client_id;
            }),
        destinations_.end());
    if (destinations_.size() == before) {
        return;
    }
    if (destinations_.empty()) {
        process_.stop();
        return;
    }
    restart_pipeline();
}

void GStreamerAudioFanout::restart() {
    restart_pipeline();
}

void GStreamerAudioFanout::restart_pipeline() {
    process_.stop();
    if (destinations_.empty()) {
        return;
    }
    for (const auto& destination : destinations_) {
        terminate_gst_multiudpsink_on_port(destination.port);
    }
    if (!gst_element_available("multiudpsink")) {
        throw std::runtime_error("multiudpsink is required for shared audio fanout (gst-plugins-good)");
    }

    std::vector<std::pair<std::string, std::uint16_t>> clients;
    clients.reserve(destinations_.size());
    for (const auto& destination : destinations_) {
        clients.emplace_back(destination.host, destination.port);
    }

    auto args = std::vector<std::string>{
        "gst-launch-1.0",
        "-q",
    };
    // Pulse-style "*.monitor" names (e.g. archstreamer.monitor) must use pulsesrc.
    // pipewiresrc used to fall through to @DEFAULT_MONITOR@ whenever the name
    // contained ".monitor", which captured the wrong sink (HDMI/etc.) while
    // RetroArch played into the silent null sink — late/missing client audio and
    // audio_sync pacing stalls on the host (Space=FF looked like a "video unstick").
    const bool pulse_monitor =
        !source_.empty() && source_.size() > 8 &&
        source_.compare(source_.size() - 8, 8, ".monitor") == 0;
    if (backend_ == AudioCaptureBackend::Pulse || pulse_monitor) {
        args.push_back("pulsesrc");
        args.push_back("client-name=ArchStreamer");
        args.push_back("do-timestamp=true");
        // Keep capture latency tight so the null-sink monitor stays awake and
        // RetroArch's audio_sync clock does not build a multi-second backlog.
        args.push_back("buffer-time=80000");
        args.push_back("latency-time=20000");
        args.push_back("provide-clock=false");
        if (!source_.empty()) {
            args.push_back("device=" + source_);
        }
    } else {
        args.push_back("pipewiresrc");
        args.push_back("client-name=ArchStreamer");
        args.push_back("do-timestamp=true");
        if (!source_.empty()) {
            args.push_back("target-object=" + source_);
        } else {
            args.push_back("target-object=@DEFAULT_MONITOR@");
        }
    }
    args.insert(args.end(), {
        "!",
        "audioconvert",
        "!",
        "audioresample",
        "!",
        "audio/x-raw,rate=48000,channels=2",
        "!",
        "opusenc",
        "bitrate=128000",
        "frame-size=20",
        "inband-fec=true",
        "!",
        "rtpopuspay",
        "pt=97",
        "!",
        "multiudpsink",
        "clients=" + multiudp_clients_arg(clients),
        "sync=false",
        "async=false",
    });
    process_.start(std::move(args));
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    if (!process_.running()) {
        throw std::runtime_error(
            "audio capture pipeline exited immediately (need pulsesrc/pipewiresrc, opusenc, multiudpsink)");
    }
    std::cout
        << "Audio capture (shared): " << destinations_.size()
        << " destination(s) from "
        << (source_.empty() ? std::string("<default>") : source_)
        << '\n';
}

} // namespace archstreamer
