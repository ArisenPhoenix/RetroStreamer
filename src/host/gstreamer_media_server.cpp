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

namespace {

using SinkInputMatchFn = bool (*)(const std::string&);

int move_matching_sink_inputs_to(const char* destination_sink, SinkInputMatchFn matches) {
    const auto dump = archstreamer::read_command_output("pactl list sink-inputs 2>/dev/null");
    if (dump.empty()) {
        return 0;
    }

    int moved = 0;
    std::string::size_type pos = 0;
    while (pos < dump.size()) {
        const auto next = dump.find("Sink Input #", pos + 1);
        const auto block = dump.substr(
            pos,
            next == std::string::npos ? std::string::npos : next - pos);
        pos = next == std::string::npos ? dump.size() : next;

        if (!matches(block)) {
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
            (std::string("pactl move-sink-input ") + id + " " + destination_sink +
             " 2>/dev/null && echo ok")
                .c_str());
        if (result.find("ok") != std::string::npos) {
            ++moved;
        }
    }
    return moved;
}

bool block_looks_like_retroarch(const std::string& block) {
    return block.find("application.process.binary = \"retroarch\"") != std::string::npos ||
        block.find("application.name = \"RetroArch\"") != std::string::npos ||
        block.find("node.name = \"RetroArch\"") != std::string::npos;
}

} // namespace

void park_streaming_game_audio() {
    // Viewer RetroArch must stay on the silent null sink. PipeWire stream-restore can
    // reattach it to HDMI/USB after a prior move, which leaks game audio to speakers
    // even when Watch-local is off (Watch is the only intentional local listen path).
    try {
        ensure_streaming_audio_sink();
    } catch (const std::exception& error) {
        std::cerr << "Warning: could not ensure streaming audio sink: " << error.what() << '\n';
        return;
    }

    const auto moved = move_matching_sink_inputs_to(
        kStreamingAudioSinkName,
        block_looks_like_retroarch);
    if (moved > 0) {
        std::cout
            << "Parked " << moved
            << " RetroArch stream(s) on '" << kStreamingAudioSinkName
            << "' (speakers stay quiet unless Watch stream locally).\n";
    }
}

void restore_default_sink_after_streaming() {
    // Never leave the session default on the silent capture sink — that mutes desktop
    // audio until the user notices.
    const auto current = archstreamer::read_command_output("pactl get-default-sink 2>/dev/null");
    if (current != kStreamingAudioSinkName) {
        return;
    }

    const auto sinks = archstreamer::read_command_output("pactl list short sinks 2>/dev/null");
    std::string::size_type pos = 0;
    while (pos < sinks.size()) {
        const auto end = sinks.find('\n', pos);
        const auto line = sinks.substr(
            pos,
            end == std::string::npos ? std::string::npos : end - pos);
        pos = end == std::string::npos ? sinks.size() : end + 1;
        if (line.empty()) {
            continue;
        }
        const auto first_tab = line.find('\t');
        if (first_tab == std::string::npos) {
            continue;
        }
        auto rest = line.substr(first_tab + 1);
        const auto second_tab = rest.find('\t');
        const auto name = second_tab == std::string::npos ? rest : rest.substr(0, second_tab);
        if (name.empty() || name == kStreamingAudioSinkName) {
            continue;
        }
        const auto result = archstreamer::read_command_output(
            (std::string("pactl set-default-sink ") + name + " 2>/dev/null && echo ok").c_str());
        if (result.find("ok") != std::string::npos) {
            std::cout << "Restored default sink to '" << name << "' after streaming session.\n";
            return;
        }
    }
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

    source_kind_ = SourceKind::X11;
    display_ = display;
    pipewire_node_.clear();
    auto streams = std::vector<MediaClientStream>{};
    streams.reserve(destinations.size());
    for (const auto& destination : destinations) {
        Destination slot{};
        slot.client_id = destination.client_id;
        slot.host = destination.destination_host;
        slot.port = destination.port;
        slot.tier = MediaQualityTier::Medium;
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

std::vector<MediaClientStream> GStreamerVideoFanout::start_pipewire(
    const std::string& pipewire_node,
    const std::vector<MediaStreamRequest>& destinations) {
    if (!destinations_.empty() || process_.running()) {
        throw std::runtime_error("video fanout is already running");
    }

    source_kind_ = SourceKind::PipeWire;
    display_.clear();
    pipewire_node_ = pipewire_node;
    auto streams = std::vector<MediaClientStream>{};
    streams.reserve(destinations.size());
    for (const auto& destination : destinations) {
        Destination slot{};
        slot.client_id = destination.client_id;
        slot.host = destination.destination_host;
        slot.port = destination.port;
        slot.tier = MediaQualityTier::Medium;
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
    slot.tier = settings.bitrate_kbps == 0
        ? MediaQualityTier::Medium
        : media_quality_tier_for_settings(settings);
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
    if (slot == nullptr) {
        return false;
    }
    if (source_kind_ == SourceKind::X11 && display_.empty()) {
        return false;
    }
    if (source_kind_ == SourceKind::PipeWire && pipewire_node_.empty()) {
        return false;
    }
    const auto tier = media_quality_tier_for_settings(settings);
    if (slot->tier == tier) {
        return false;
    }
    slot->tier = tier;
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
    if (destinations_.empty()) {
        return;
    }
    if (source_kind_ == SourceKind::X11 && display_.empty()) {
        return;
    }
    if (source_kind_ == SourceKind::PipeWire && pipewire_node_.empty()) {
        return;
    }
    if (!gst_element_available("multiudpsink")) {
        throw std::runtime_error("multiudpsink is required for video ladder fanout (gst-plugins-good)");
    }
    if (source_kind_ == SourceKind::PipeWire && !gst_element_available("pipewiresrc")) {
        throw std::runtime_error("pipewiresrc is required for gamescope video capture (gst-plugin-pipewire)");
    }

    std::vector<std::pair<std::string, std::uint16_t>> high_clients;
    std::vector<std::pair<std::string, std::uint16_t>> medium_clients;
    std::vector<std::pair<std::string, std::uint16_t>> low_clients;
    for (const auto& destination : destinations_) {
        const auto client = std::make_pair(destination.host, destination.port);
        switch (destination.tier) {
        case MediaQualityTier::Low:
            low_clients.push_back(client);
            break;
        case MediaQualityTier::Medium:
        case MediaQualityTier::Auto:
            medium_clients.push_back(client);
            break;
        case MediaQualityTier::High:
        default:
            high_clients.push_back(client);
            break;
        }
    }

    const int active_tiers =
        (high_clients.empty() ? 0 : 1) +
        (medium_clients.empty() ? 0 : 1) +
        (low_clients.empty() ? 0 : 1);
    if (active_tiers == 0) {
        return;
    }

    auto append_h264_branch = [&](
        std::vector<std::string>& args,
        const VideoEncodeSettings& settings,
        const std::vector<std::pair<std::string, std::uint16_t>>& clients,
        bool use_nvenc) {
        const auto bitrate = settings.bitrate_kbps == 0 ? 1500 : settings.bitrate_kbps;
        const auto framerate = settings.framerate == 0 ? 30 : settings.framerate;
        const auto key_int_max = settings.key_int_max == 0 ? framerate : settings.key_int_max;

        args.insert(args.end(), {
            "queue",
            "max-size-buffers=3",
            "leaky=downstream",
            "!",
            "videorate",
            "drop-only=true",
            "!",
            "video/x-raw,framerate=" + std::to_string(framerate) + "/1",
            "!",
        });
        if (use_nvenc) {
            args.insert(args.end(), {
                "nvh264enc",
                "zerolatency=true",
                "preset=low-latency-hq",
                "bitrate=" + std::to_string(bitrate),
                "gop-size=" + std::to_string(key_int_max),
                "!",
                "video/x-h264,profile=baseline,stream-format=byte-stream",
            });
        } else {
            args.insert(args.end(), {
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
            });
        }
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
    };

    const bool nvenc = gst_element_available("nvh264enc");
    auto args = std::vector<std::string>{"gst-launch-1.0", "-q"};
    if (source_kind_ == SourceKind::PipeWire) {
        args.insert(args.end(), {
            "pipewiresrc",
            "path=" + pipewire_node_,
            "do-timestamp=true",
            "!",
            "video/x-raw,format=BGRx",
            "!",
            "videoconvert",
        });
    } else {
        args.insert(args.end(), {
            "ximagesrc",
            "display-name=" + display_,
            "use-damage=0",
            "show-pointer=false",
            "!",
            "videoconvert",
        });
    }

    if (active_tiers == 1) {
        // Single tier: no tee needed.
        const auto* clients = !high_clients.empty() ? &high_clients
            : !medium_clients.empty() ? &medium_clients
            : &low_clients;
        const auto tier = !high_clients.empty() ? MediaQualityTier::High
            : !medium_clients.empty() ? MediaQualityTier::Medium
            : MediaQualityTier::Low;
        const auto settings = video_encode_settings_for_tier(tier);
        args.push_back("!");
        append_h264_branch(args, settings, *clients, nvenc && tier == MediaQualityTier::High);
    } else {
        args.insert(args.end(), {"!", "tee", "name=t"});
        auto append_tier = [&](
            const std::vector<std::pair<std::string, std::uint16_t>>& clients,
            MediaQualityTier tier) {
            if (clients.empty()) {
                return;
            }
            args.push_back("t.");
            args.push_back("!");
            append_h264_branch(
                args,
                video_encode_settings_for_tier(tier),
                clients,
                nvenc && tier == MediaQualityTier::High);
        };
        append_tier(high_clients, MediaQualityTier::High);
        append_tier(medium_clients, MediaQualityTier::Medium);
        append_tier(low_clients, MediaQualityTier::Low);
    }

    process_.start(std::move(args));
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    if (!process_.running()) {
        throw std::runtime_error(
            source_kind_ == SourceKind::PipeWire
                ? "video capture pipeline exited immediately (need pipewiresrc, x264enc/nvh264enc, multiudpsink)"
                : "video capture pipeline exited immediately (need Xvfb/Xephyr, ximagesrc, x264enc, multiudpsink)");
    }

    std::cout << "Video ladder ("
              << (source_kind_ == SourceKind::PipeWire ? "pipewire" : "ximagesrc")
              << (nvenc ? ", nvenc" : ", x264")
              << "):";
    if (!high_clients.empty()) {
        const auto s = video_encode_settings_for_tier(MediaQualityTier::High);
        std::cout << " high=" << high_clients.size()
                  << "@" << s.bitrate_kbps << "kbps/" << static_cast<int>(s.framerate) << "fps";
    }
    if (!medium_clients.empty()) {
        const auto s = video_encode_settings_for_tier(MediaQualityTier::Medium);
        std::cout << " med=" << medium_clients.size()
                  << "@" << s.bitrate_kbps << "kbps/" << static_cast<int>(s.framerate) << "fps";
    }
    if (!low_clients.empty()) {
        const auto s = video_encode_settings_for_tier(MediaQualityTier::Low);
        std::cout << " low=" << low_clients.size()
                  << "@" << s.bitrate_kbps << "kbps/" << static_cast<int>(s.framerate) << "fps";
    }
    std::cout << '\n';
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
    destinations_ = destinations;
    if (capture_.video) {
        virtual_display_ = make_virtual_display(capture_.display_backend);
        if (virtual_display_) {
            virtual_display_->start(capture_.virtual_display, capture_.video_resolution);
            if (capture_.verbose) {
                std::cout << "Virtual display backend=";
                switch (virtual_display_->backend()) {
                case VirtualDisplayBackend::Gamescope:
                    std::cout << "gamescope (headless + PipeWire)";
                    break;
                case VirtualDisplayBackend::VirtualGL:
                    std::cout << "virtualgl (Xvfb + vglrun)";
                    break;
                case VirtualDisplayBackend::Xephyr:
                    std::cout << "xephyr on " << capture_.virtual_display;
                    break;
                case VirtualDisplayBackend::Xvfb:
                    std::cout << "xvfb on " << capture_.virtual_display;
                    break;
                case VirtualDisplayBackend::None:
                    std::cout << "none";
                    break;
                }
                std::cout << '\n';
            }
        }

        defer_pipewire_video_ =
            virtual_display_ && virtual_display_->uses_pipewire_video();
        if (defer_pipewire_video_) {
            // Assign RTP URIs now; attach pipewiresrc after gamescope publishes its node.
            const auto requests = video_requests_from_media_destinations(plan, destinations);
            for (const auto& request : requests) {
                for (auto& media_stream : streams) {
                    if (media_stream.client_id == request.client_id) {
                        media_stream.endpoint.video_uri =
                            rtp_h264_uri(request.destination_host, request.port);
                    }
                }
            }
            if (capture_.verbose) {
                std::cout << "Video capture deferred until gamescope PipeWire node is ready.\n";
            }
        } else {
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

bool GStreamerMediaServer::video_deferred() const {
    return defer_pipewire_video_ && !video_fanout_.has_value();
}

void GStreamerMediaServer::start_pipewire_video(
    const std::string& pipewire_node,
    std::vector<MediaClientStream>& streams) {
    if (!capture_.video || pipewire_node.empty()) {
        return;
    }
    if (video_fanout_.has_value()) {
        video_fanout_->stop();
        video_fanout_.reset();
    }
    video_fanout_.emplace();
    const auto video_streams = video_fanout_->start_pipewire(
        pipewire_node,
        video_requests_from_media_destinations(plan_, destinations_));
    for (const auto& stream : video_streams) {
        for (auto& media_stream : streams) {
            if (media_stream.client_id == stream.client_id) {
                media_stream.endpoint.video_uri = stream.endpoint.video_uri;
            }
        }
    }
    defer_pipewire_video_ = false;
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
    if (virtual_display_) {
        virtual_display_->stop();
        virtual_display_.reset();
    }
}

std::unique_ptr<MediaServer> make_gstreamer_media_server(const GStreamerMediaCaptureConfig& capture) {
    return std::make_unique<GStreamerMediaServer>(capture);
}

} // namespace archstreamer
