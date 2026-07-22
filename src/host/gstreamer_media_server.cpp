#include "common/addresses.hpp"
#include "common/platform/process_utils.hpp"
#include "host/gstreamer_media_server.hpp"
#include "host/host_launch_planner.hpp"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>
#include <utility>

#include <unistd.h>

namespace archstreamer {

bool command_available(const char* command) {
    const auto check_path = [command](const std::filesystem::path& directory) {
        if (directory.empty()) {
            return false;
        }
        const auto path = directory / command;
        return access(path.c_str(), X_OK) == 0;
    };

    const char* path_env = std::getenv("PATH");
    if (path_env != nullptr) {
        auto paths = std::string{path_env};
        std::string::size_type start = 0;
        while (start <= paths.size()) {
            const auto end = paths.find(':', start);
            const auto directory = paths.substr(start, end == std::string::npos ? std::string::npos : end - start);
            if (check_path(directory)) {
                return true;
            }
            if (end == std::string::npos) {
                break;
            }
            start = end + 1;
        }
    }

    return check_path("/usr/bin") || check_path("/usr/local/bin");
}

VirtualDisplayBackend choose_virtual_display_backend(VirtualDisplayBackend requested) {
    if (requested != VirtualDisplayBackend::None) {
        return requested;
    }
    if (command_available("Xvfb")) {
        return VirtualDisplayBackend::Xvfb;
    }
    if (command_available("Xephyr")) {
        return VirtualDisplayBackend::Xephyr;
    }

    throw std::runtime_error("no virtual display backend found; install Xvfb or Xephyr");
}

std::string trim_command_output(std::string value) {
    return trim_ascii_whitespace(std::move(value));
}

AudioCaptureBackend choose_audio_capture_backend(AudioCaptureBackend requested) {
    // Keep Pulse as the auto default. We resolve monitors via pactl (…sink.monitor), which
    // pulsesrc understands on PipeWire-with-Pulse. Auto-picking pipewiresrc with that name
    // fails with "target not found".
    return requested;
}

std::string default_audio_monitor_source() {
    const auto sink = archstreamer::read_command_output("pactl get-default-sink 2>/dev/null");
    if (sink.empty()) {
        return {};
    }

    return sink + ".monitor";
}

VirtualDisplayProcess::~VirtualDisplayProcess() {
    stop();
}

void VirtualDisplayProcess::start(
    VirtualDisplayBackend backend,
    const std::string& display,
    const std::string& resolution) {
    backend_ = choose_virtual_display_backend(backend);
    if (backend_ == VirtualDisplayBackend::Xvfb) {
        process_.start({"Xvfb", display, "-screen", "0", resolution + "x24", "-nolisten", "tcp"});
    } else if (backend_ == VirtualDisplayBackend::Xephyr) {
        process_.start({"Xephyr", display, "-screen", resolution, "-ac", "-noreset"});
    } else {
        return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(750));
}

void VirtualDisplayProcess::stop() {
    process_.stop();
}

MediaEndpoint GStreamerVideoSender::endpoint(std::string host, std::uint16_t port) const {
    return MediaEndpoint{
        rtp_h264_uri(std::move(host), port),
        "",
    };
}

void GStreamerVideoSender::start(
    const std::string& display,
    const std::string& destination_host,
    std::uint16_t port) {
    auto args = std::vector<std::string>{
        "gst-launch-1.0",
        "-q",
        "ximagesrc",
        "display-name=" + display,
        "use-damage=0",
        "show-pointer=false",
        "!",
        "video/x-raw,framerate=30/1",
        "!",
        "videoconvert",
        "!",
        "queue",
        "!",
        // Constrained baseline + byte-stream so Flatpak openh264dec can decode.
        "x264enc",
        "tune=zerolatency",
        "speed-preset=ultrafast",
        "bitrate=1500",
        "key-int-max=30",
        "byte-stream=true",
        "bframes=0",
        "threads=1",
        "!",
        "video/x-h264,profile=constrained-baseline,stream-format=byte-stream",
    };
    // h264parse is ideal but not always installed (gst-plugins-bad).
    if (command_available("gst-inspect-1.0")) {
        if (std::system("gst-inspect-1.0 h264parse >/dev/null 2>&1") == 0) {
            args.insert(args.end(), {"!", "h264parse", "config-interval=-1"});
        }
    }
    args.insert(args.end(), {
        "!",
        "rtph264pay",
        // Keep RTP under typical Wi‑Fi/VPN MTUs; oversized video datagrams are often
        // dropped silently while small Opus audio packets still arrive.
        "mtu=1200",
        "config-interval=1",
        "pt=96",
        "!",
        "udpsink",
        "host=" + destination_host,
        "port=" + std::to_string(port),
        "sync=false",
        "async=false",
    });
    process_.start(std::move(args));
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    if (!process_.running()) {
        throw std::runtime_error(
            "video capture pipeline exited immediately (need Xvfb/Xephyr, ximagesrc, x264enc)");
    }
    std::cout
        << "Video capture running to " << destination_host << ':' << port
        << " (H.264 baseline, RTP mtu=1200)\n";
}

void GStreamerVideoSender::stop() {
    process_.stop();
}

GStreamerVideoFanout::~GStreamerVideoFanout() {
    stop();
}

std::vector<MediaClientStream> GStreamerVideoFanout::start(
    const std::string& display,
    const std::vector<MediaStreamRequest>& destinations) {
    if (!senders_.empty()) {
        throw std::runtime_error("video fanout is already running");
    }

    auto streams = std::vector<MediaClientStream>{};
    streams.reserve(destinations.size());

    for (const auto& destination : destinations) {
        streams.push_back(add(display, destination));
    }

    return streams;
}

MediaClientStream GStreamerVideoFanout::add(
    const std::string& display,
    const MediaStreamRequest& destination) {
    auto sender = GStreamerVideoSender{};
    const auto endpoint = sender.endpoint(destination.destination_host, destination.port);
    sender.start(display, destination.destination_host, destination.port);
    stop_client(destination.client_id);
    senders_.emplace(destination.client_id, std::move(sender));
    return MediaClientStream{
        destination.client_id,
        destination.destination_host,
        endpoint,
    };
}

void GStreamerVideoFanout::stop() {
    for (auto& [client_id, sender] : senders_) {
        (void)client_id;
        sender.stop();
    }
    senders_.clear();
}

void GStreamerVideoFanout::stop_client(ClientId client_id) {
    const auto sender = senders_.find(client_id);
    if (sender == senders_.end()) {
        return;
    }
    sender->second.stop();
    senders_.erase(sender);
}

MediaEndpoint GStreamerAudioSender::endpoint(std::string host, std::uint16_t port) const {
    return MediaEndpoint{
        "",
        rtp_opus_uri(std::move(host), port),
    };
}

void GStreamerAudioSender::start(
    AudioCaptureBackend backend,
    const std::string& source,
    const std::string& destination_host,
    std::uint16_t port) {
    auto args = std::vector<std::string>{
        "gst-launch-1.0",
        "-q",
    };

    if (backend == AudioCaptureBackend::Pulse) {
        args.push_back("pulsesrc");
        args.push_back("client-name=ArchStreamer");
        args.push_back("do-timestamp=true");
        if (!source.empty()) {
            args.push_back("device=" + source);
        }
    } else {
        args.push_back("pipewiresrc");
        args.push_back("client-name=ArchStreamer");
        args.push_back("do-timestamp=true");
        // Pulse-style "…sink.monitor" names are not valid pipewiresrc targets.
        if (!source.empty() && source.find(".monitor") == std::string::npos) {
            args.push_back("target-object=" + source);
        } else {
            args.push_back("target-object=@DEFAULT_MONITOR@");
        }
    }

    const auto tail = std::vector<std::string>{
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
        "udpsink",
        "host=" + destination_host,
        "port=" + std::to_string(port),
        "sync=false",
        "async=false",
    };
    args.insert(args.end(), tail.begin(), tail.end());
    process_.start(std::move(args));
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    if (!process_.running()) {
        throw std::runtime_error(
            "audio capture pipeline exited immediately (need pulsesrc/pipewiresrc and opusenc)");
    }
}

void GStreamerAudioSender::stop() {
    process_.stop();
}

GStreamerAudioFanout::~GStreamerAudioFanout() {
    stop();
}

std::vector<MediaClientStream> GStreamerAudioFanout::start(
    AudioCaptureBackend backend,
    const std::string& source,
    const std::vector<MediaStreamRequest>& destinations) {
    if (!senders_.empty()) {
        throw std::runtime_error("audio fanout is already running");
    }

    auto streams = std::vector<MediaClientStream>{};
    streams.reserve(destinations.size());

    for (const auto& destination : destinations) {
        streams.push_back(add(backend, source, destination));
    }

    return streams;
}

MediaClientStream GStreamerAudioFanout::add(
    AudioCaptureBackend backend,
    const std::string& source,
    const MediaStreamRequest& destination) {
    auto sender = GStreamerAudioSender{};
    const auto endpoint = sender.endpoint(destination.destination_host, destination.port);
    sender.start(backend, source, destination.destination_host, destination.port);
    stop_client(destination.client_id);
    senders_.emplace(destination.client_id, std::move(sender));
    return MediaClientStream{
        destination.client_id,
        destination.destination_host,
        endpoint,
    };
}

void GStreamerAudioFanout::stop() {
    for (auto& [client_id, sender] : senders_) {
        (void)client_id;
        sender.stop();
    }
    senders_.clear();
}

void GStreamerAudioFanout::stop_client(ClientId client_id) {
    const auto sender = senders_.find(client_id);
    if (sender == senders_.end()) {
        return;
    }
    sender->second.stop();
    senders_.erase(sender);
}

GStreamerMediaServer::GStreamerMediaServer(GStreamerMediaCaptureConfig capture)
    : capture_(std::move(capture)) {
}

void GStreamerMediaServer::start(
    const HostMediaPlanConfig& plan,
    const std::vector<HostMediaDestination>& destinations,
    std::vector<MediaClientStream>& streams) {
    plan_ = plan;
    if (capture_.video) {
        virtual_display_.emplace();
        virtual_display_->start(capture_.display_backend, capture_.virtual_display, capture_.video_resolution);
        video_fanout_.emplace();
        const auto video_streams = video_fanout_->start(
            capture_.virtual_display,
            video_requests_from_media_destinations(plan, destinations));
        for (const auto& stream : video_streams) {
            for (auto& media_stream : streams) {
                if (media_stream.client_id == stream.client_id) {
                    media_stream.endpoint.video_uri = stream.endpoint.video_uri;
                }
            }
        }
    }
    if (capture_.audio) {
        try {
            audio_fanout_.emplace();
            const auto audio_streams = audio_fanout_->start(
                capture_.audio_backend,
                capture_.audio_source,
                audio_requests_from_media_destinations(plan, destinations));
            for (const auto& stream : audio_streams) {
                for (auto& media_stream : streams) {
                    if (media_stream.client_id == stream.client_id) {
                        media_stream.endpoint.audio_uri = stream.endpoint.audio_uri;
                    }
                }
            }
        } catch (const std::exception& error) {
            audio_fanout_.reset();
            capture_.audio = false;
            std::cerr << "Warning: audio streaming disabled: " << error.what() << '\n';
        }
    }
}

MediaEndpoint GStreamerMediaServer::add_client(
    ClientId client_id,
    const std::string& destination_host,
    std::size_t media_index,
    bool wants_video,
    bool wants_audio) {
    auto endpoint = MediaEndpoint{};
    const auto destination = HostMediaDestination{client_id, destination_host};
    if (wants_video && capture_.video && video_fanout_.has_value()) {
        const auto stream = video_fanout_->add(
            capture_.virtual_display,
            video_request_for_destination(plan_, destination, media_index));
        endpoint.video_uri = stream.endpoint.video_uri;
    }
    if (wants_audio && capture_.audio && audio_fanout_.has_value()) {
        const auto stream = audio_fanout_->add(
            capture_.audio_backend,
            capture_.audio_source,
            audio_request_for_destination(plan_, destination, media_index));
        endpoint.audio_uri = stream.endpoint.audio_uri;
    }
    return endpoint;
}

void GStreamerMediaServer::remove_client(ClientId client_id) {
    if (video_fanout_.has_value()) {
        video_fanout_->stop_client(client_id);
    }
    if (audio_fanout_.has_value()) {
        audio_fanout_->stop_client(client_id);
    }
}

void GStreamerMediaServer::stop() {
    if (audio_fanout_.has_value()) {
        audio_fanout_->stop();
        audio_fanout_.reset();
    }
    if (video_fanout_.has_value()) {
        video_fanout_->stop();
        video_fanout_.reset();
    }
    if (virtual_display_.has_value()) {
        virtual_display_->stop();
        virtual_display_.reset();
    }
}

std::unique_ptr<MediaServer> make_gstreamer_media_server(const GStreamerMediaCaptureConfig& capture) {
    return std::make_unique<GStreamerMediaServer>(capture);
}

} // namespace archstreamer
