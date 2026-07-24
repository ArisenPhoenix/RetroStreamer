#include "common/addresses.hpp"
#include "common/platform/process_utils.hpp"
#include "host/gstreamer_media_server.hpp"
#include "host/host_launch_planner.hpp"

#include <array>
#include <algorithm>
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

namespace {

constexpr const char* kStreamingAudioSinkName = "archstreamer";
constexpr const char* kVmAudioParkSinkName = "archstreamer-vm";

bool sink_exists(const std::string& sink_name) {
    const auto sinks = archstreamer::read_command_output("pactl list short sinks 2>/dev/null");
    if (sinks.empty()) {
        return false;
    }
    // Match a whole sink name column (name is the second whitespace-separated field).
    std::string::size_type line_start = 0;
    while (line_start < sinks.size()) {
        const auto line_end = sinks.find('\n', line_start);
        const auto line = sinks.substr(
            line_start,
            line_end == std::string::npos ? std::string::npos : line_end - line_start);
        line_start = line_end == std::string::npos ? sinks.size() : line_end + 1;

        std::string::size_type field = 0;
        std::string::size_type pos = 0;
        while (pos < line.size()) {
            while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t')) {
                ++pos;
            }
            if (pos >= line.size()) {
                break;
            }
            const auto end = line.find_first_of(" \t", pos);
            const auto token = line.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
            if (field == 1 && token == sink_name) {
                return true;
            }
            ++field;
            if (end == std::string::npos) {
                break;
            }
            pos = end;
        }
    }
    return false;
}

} // namespace

std::string ensure_named_null_sink(const char* sink_name, const char* description) {
    if (!sink_exists(sink_name)) {
        const auto module = archstreamer::read_command_output(
            (std::string("pactl load-module module-null-sink sink_name=") + sink_name +
             " sink_properties=device.description=\"" + description +
             "\",session.suspend-timeout.seconds=0 2>/dev/null")
                .c_str());
        if (module.empty() || !sink_exists(sink_name)) {
            throw std::runtime_error(
                std::string("failed to create null sink '") + sink_name +
                "' (need pactl / module-null-sink)");
        }
    }
    (void)archstreamer::read_command_output(
        (std::string("pactl suspend-sink ") + sink_name + " 0 2>/dev/null").c_str());
    return sink_name;
}

std::string ensure_streaming_audio_sink() {
    return ensure_named_null_sink(
        kStreamingAudioSinkName,
        "ArchStreamer");
}

std::string streaming_audio_monitor_source() {
    return ensure_streaming_audio_sink() + ".monitor";
}

void park_vm_host_audio_streams() {
    // Best-effort: QEMU spice/PipeWire playback on the metal host (see
    // deploy/vm-client/fix-host-vm-audio.sh) shares the default sink with Host Player
    // and with "Watch stream locally", at 44100 Hz vs RetroArch's 48000 — that mix is
    // a common source of muddy/crackly metal-host audio.
    try {
        ensure_named_null_sink(kVmAudioParkSinkName, "ArchStreamer-VM-park");
    } catch (const std::exception& error) {
        std::cerr << "Warning: could not create VM audio park sink: " << error.what() << '\n';
        return;
    }

    const auto dump = archstreamer::read_command_output("pactl list sink-inputs 2>/dev/null");
    if (dump.empty()) {
        return;
    }

    int moved = 0;
    std::string::size_type pos = 0;
    while (pos < dump.size()) {
        const auto next = dump.find("Sink Input #", pos + 1);
        const auto block = dump.substr(
            pos,
            next == std::string::npos ? std::string::npos : next - pos);
        pos = next == std::string::npos ? dump.size() : next;

        if (block.find("qemu-system-x86_64") == std::string::npos &&
            block.find("spice") == std::string::npos) {
            continue;
        }
        const auto hash = block.find('#');
        if (hash == std::string::npos) {
            continue;
        }
        const auto id_end = block.find_first_not_of("0123456789", hash + 1);
        const auto id = block.substr(
            hash + 1,
            (id_end == std::string::npos ? block.size() : id_end) - (hash + 1));
        if (id.empty()) {
            continue;
        }
        const auto result = archstreamer::read_command_output(
            (std::string("pactl move-sink-input ") + id + " " + kVmAudioParkSinkName +
             " 2>/dev/null && echo ok")
                .c_str());
        if (result.find("ok") != std::string::npos) {
            ++moved;
        }
    }
    if (moved > 0) {
        std::cout
            << "Parked " << moved
            << " VM host-audio stream(s) on '" << kVmAudioParkSinkName
            << "' so they do not share the game playback sink.\n";
    }
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
    if (!command_available("gst-inspect-1.0")) {
        return false;
    }
    return std::system(
               (std::string("gst-inspect-1.0 ") + element + " >/dev/null 2>&1").c_str()) == 0;
}

} // namespace

GStreamerVideoFanout::~GStreamerVideoFanout() {
    stop();
}

std::vector<MediaClientStream> GStreamerVideoFanout::start(
    const std::string& display,
    const std::vector<MediaStreamRequest>& destinations) {
    if (!destinations_.empty() || process_.running()) {
        throw std::runtime_error("video fanout is already running");
    }

    display_ = display;
    shared_settings_ = video_encode_settings_for_tier(MediaQualityTier::High);
    auto streams = std::vector<MediaClientStream>{};
    streams.reserve(destinations.size());
    for (const auto& destination : destinations) {
        Destination slot{};
        slot.client_id = destination.client_id;
        slot.host = destination.destination_host;
        slot.port = destination.port;
        slot.settings = shared_settings_;
        destinations_.push_back(slot);
        streams.push_back(MediaClientStream{
            destination.client_id,
            destination.destination_host,
            MediaEndpoint{rtp_h264_uri(destination.destination_host, destination.port), ""},
        });
    }
    if (!destinations_.empty()) {
        restart_pipeline();
    }
    return streams;
}

MediaClientStream GStreamerVideoFanout::add(
    const std::string& display,
    const MediaStreamRequest& destination,
    const VideoEncodeSettings& settings) {
    display_ = display;
    stop_client(destination.client_id);

    Destination slot{};
    slot.client_id = destination.client_id;
    slot.host = destination.destination_host;
    slot.port = destination.port;
    slot.settings = settings.bitrate_kbps == 0
        ? video_encode_settings_for_tier(MediaQualityTier::High)
        : settings;
    // Shared encode: keep the most demanding (lowest bitrate) tier among clients.
    if (destinations_.empty()) {
        shared_settings_ = slot.settings;
    } else if (slot.settings.bitrate_kbps < shared_settings_.bitrate_kbps) {
        shared_settings_ = slot.settings;
    }
    destinations_.push_back(std::move(slot));
    restart_pipeline();

    return MediaClientStream{
        destination.client_id,
        destination.destination_host,
        MediaEndpoint{rtp_h264_uri(destination.destination_host, destination.port), ""},
    };
}

bool GStreamerVideoFanout::reconfigure_client(ClientId client_id, const VideoEncodeSettings& settings) {
    Destination* slot = nullptr;
    for (auto& destination : destinations_) {
        if (destination.client_id == client_id) {
            slot = &destination;
            break;
        }
    }
    if (slot == nullptr || display_.empty()) {
        return false;
    }
    if (slot->settings.bitrate_kbps == settings.bitrate_kbps &&
        slot->settings.framerate == settings.framerate &&
        slot->settings.key_int_max == settings.key_int_max) {
        return false;
    }
    slot->settings = settings;

    VideoEncodeSettings next = destinations_.front().settings;
    for (const auto& destination : destinations_) {
        if (destination.settings.bitrate_kbps < next.bitrate_kbps) {
            next = destination.settings;
        }
    }
    if (shared_settings_.bitrate_kbps == next.bitrate_kbps &&
        shared_settings_.framerate == next.framerate &&
        shared_settings_.key_int_max == next.key_int_max) {
        return false;
    }
    shared_settings_ = next;
    restart_pipeline();
    return true;
}

void GStreamerVideoFanout::stop() {
    process_.stop();
    destinations_.clear();
    display_.clear();
}

void GStreamerVideoFanout::stop_client(ClientId client_id) {
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

void GStreamerVideoFanout::restart_pipeline() {
    process_.stop();
    if (display_.empty() || destinations_.empty()) {
        return;
    }
    if (!gst_element_available("multiudpsink")) {
        throw std::runtime_error("multiudpsink is required for shared video fanout (gst-plugins-good)");
    }

    const auto bitrate = shared_settings_.bitrate_kbps == 0 ? 1500 : shared_settings_.bitrate_kbps;
    const auto framerate = shared_settings_.framerate == 0 ? 30 : shared_settings_.framerate;
    const auto key_int_max =
        shared_settings_.key_int_max == 0 ? framerate : shared_settings_.key_int_max;

    std::vector<std::pair<std::string, std::uint16_t>> clients;
    clients.reserve(destinations_.size());
    for (const auto& destination : destinations_) {
        clients.emplace_back(destination.host, destination.port);
    }

    auto args = std::vector<std::string>{
        "gst-launch-1.0",
        "-q",
        "ximagesrc",
        "display-name=" + display_,
        "use-damage=0",
        "show-pointer=false",
        "!",
        "video/x-raw,framerate=" + std::to_string(framerate) + "/1",
        "!",
        "videoconvert",
        "!",
        "queue",
        "!",
        "x264enc",
        "tune=zerolatency",
        "speed-preset=ultrafast",
        "bitrate=" + std::to_string(bitrate),
        "key-int-max=" + std::to_string(key_int_max),
        "byte-stream=true",
        "bframes=0",
        "threads=1",
        "!",
        "video/x-h264,profile=constrained-baseline,stream-format=byte-stream",
    };
    if (gst_element_available("h264parse")) {
        args.insert(args.end(), {"!", "h264parse", "config-interval=-1"});
    }
    args.insert(args.end(), {
        "!",
        "rtph264pay",
        "mtu=1200",
        "config-interval=1",
        "pt=96",
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
            "video capture pipeline exited immediately (need Xvfb/Xephyr, ximagesrc, x264enc, multiudpsink)");
    }
    std::cout
        << "Video capture (shared): " << destinations_.size() << " destination(s), "
        << bitrate << " kbps, " << static_cast<int>(framerate) << " fps\n";
}

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

void GStreamerAudioFanout::restart_pipeline() {
    process_.stop();
    if (destinations_.empty()) {
        return;
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
    if (backend_ == AudioCaptureBackend::Pulse) {
        args.push_back("pulsesrc");
        args.push_back("client-name=ArchStreamer");
        args.push_back("do-timestamp=true");
        if (!source_.empty()) {
            args.push_back("device=" + source_);
        }
    } else {
        args.push_back("pipewiresrc");
        args.push_back("client-name=ArchStreamer");
        args.push_back("do-timestamp=true");
        if (!source_.empty() && source_.find(".monitor") == std::string::npos) {
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

bool GStreamerMediaServer::reconfigure_client_video(ClientId client_id, const VideoEncodeSettings& settings) {
    if (!video_fanout_.has_value()) {
        return false;
    }
    return video_fanout_->reconfigure_client(client_id, settings);
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
