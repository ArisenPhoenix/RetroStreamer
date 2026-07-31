#include "host/windows_media_server.hpp"

#ifdef _WIN32

#include "common/addresses.hpp"
#include "common/media.hpp"
#include "common/protocol.hpp"
#include "host/host_launch_planner.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <utility>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace archstreamer {
namespace {

bool command_on_path(const char* name) {
    char found[MAX_PATH]{};
    return SearchPathA(nullptr, name, ".exe", MAX_PATH, found, nullptr) > 0;
}

std::pair<int, int> parse_resolution(const std::string& text) {
    const auto x = text.find('x');
    if (x == std::string::npos) {
        return {1920, 1080};
    }
    try {
        return {std::stoi(text.substr(0, x)), std::stoi(text.substr(x + 1))};
    } catch (...) {
        return {1920, 1080};
    }
}

void append_udp_clients(
    std::vector<std::string>& args,
    const std::vector<MediaStreamRequest>& requests) {
    args.push_back("clients=");
    bool first = true;
    std::string clients;
    for (const auto& request : requests) {
        if (!first) {
            clients.push_back(',');
        }
        first = false;
        clients += request.destination_host;
        clients.push_back(':');
        clients += std::to_string(request.port);
    }
    if (clients.empty()) {
        throw std::runtime_error("no media destinations for Windows capture fanout");
    }
    args.back() += clients;
}

} // namespace

WindowsMediaServer::WindowsMediaServer(WindowsMediaCaptureConfig capture)
    : capture_(std::move(capture)) {
}

std::unique_ptr<MediaServer> make_windows_media_server(const WindowsMediaCaptureConfig& capture) {
    return std::make_unique<WindowsMediaServer>(capture);
}

void WindowsMediaServer::restart_video() {
    if (video_running_) {
        video_process_.stop();
        video_running_ = false;
    }
    if (!capture_.video || !plan_.video) {
        return;
    }
    const auto requests = video_requests_from_media_destinations(plan_, destinations_);
    if (requests.empty()) {
        return;
    }
    if (!command_on_path("gst-launch-1.0")) {
        throw std::runtime_error(
            "gst-launch-1.0 not on PATH (install GStreamer MSVC 64-bit — deploy/windows/install-deps.ps1)");
    }

    const auto [width, height] = parse_resolution(capture_.video_resolution);
    const auto settings = video_encode_settings_for_tier(MediaQualityTier::Medium);
    const auto bitrate = settings.bitrate_kbps == 0 ? 3500 : settings.bitrate_kbps;
    const auto framerate = settings.framerate == 0 ? 30 : settings.framerate;

    std::vector<std::string> args{
        "gst-launch-1.0",
        "-q",
        "d3d11screencapturesrc",
        "!",
        "videoconvert",
        "!",
        "videoscale",
        "!",
        "video/x-raw,width=" + std::to_string(width) + ",height=" + std::to_string(height),
        "!",
        "videorate",
        "drop-only=true",
        "!",
        "video/x-raw,framerate=" + std::to_string(framerate) + "/1",
        "!",
    };
    if (capture_.nvenc_cuda_device_id >= 0) {
        args.insert(args.end(), {
            "nvh264enc",
            "zerolatency=true",
            "preset=low-latency-hp",
            "bitrate=" + std::to_string(bitrate),
            "!",
            "video/x-h264,profile=baseline,stream-format=byte-stream",
        });
    } else {
        args.insert(args.end(), {
            "x264enc",
            "tune=zerolatency",
            "speed-preset=ultrafast",
            "bitrate=" + std::to_string(bitrate),
            "byte-stream=true",
            "bframes=0",
            "!",
            "video/x-h264,profile=baseline,stream-format=byte-stream",
        });
    }
    args.insert(args.end(), {
        "!",
        "h264parse",
        "config-interval=-1",
        "!",
        "rtph264pay",
        "pt=96",
        "config-interval=1",
        "!",
        "multiudpsink",
    });
    append_udp_clients(args, requests);

    std::vector<std::pair<std::string, std::string>> env;
    if (capture_.nvenc_cuda_device_id >= 0) {
        env.emplace_back("CUDA_DEVICE_ORDER", "PCI_BUS_ID");
        env.emplace_back("CUDA_VISIBLE_DEVICES", std::to_string(capture_.nvenc_cuda_device_id));
    }
    video_process_.start(args, env);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    if (!video_process_.running()) {
        throw std::runtime_error(
            "Windows video capture exited immediately (need d3d11screencapturesrc, x264enc/nvh264enc, multiudpsink)");
    }
    video_running_ = true;
    std::cout << "Video capture (d3d11screencapturesrc"
              << (capture_.nvenc_cuda_device_id >= 0 ? ", nvenc" : ", x264")
              << "): " << width << "x" << height << "@" << framerate << "fps\n";
}

void WindowsMediaServer::restart_audio() {
    if (audio_running_) {
        audio_process_.stop();
        audio_running_ = false;
    }
    if (!capture_.audio || !plan_.audio) {
        return;
    }
    const auto requests = audio_requests_from_media_destinations(plan_, destinations_);
    if (requests.empty()) {
        return;
    }
    if (!command_on_path("gst-launch-1.0")) {
        throw std::runtime_error("gst-launch-1.0 not on PATH");
    }

    std::vector<std::string> args{
        "gst-launch-1.0",
        "-q",
        "wasapisrc",
        "loopback=true",
        "!",
        "audioconvert",
        "!",
        "audioresample",
        "!",
        "opusenc",
        "bitrate=128000",
        "!",
        "rtpopuspay",
        "pt=111",
        "!",
        "multiudpsink",
    };
    append_udp_clients(args, requests);
    audio_process_.start(args);
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    if (!audio_process_.running()) {
        throw std::runtime_error(
            "Windows audio capture exited immediately (need wasapisrc loopback, opusenc, multiudpsink)");
    }
    audio_running_ = true;
    std::cout << "Audio capture (wasapisrc loopback)\n";
}

void WindowsMediaServer::start(
    const HostMediaPlanConfig& plan,
    const std::vector<HostMediaDestination>& destinations,
    std::vector<MediaClientStream>& streams) {
    plan_ = plan;
    destinations_ = destinations;
    streams = media_streams_for_dry_run(plan_, destinations_);

    if (capture_.video) {
        restart_video();
        for (const auto& request : video_requests_from_media_destinations(plan_, destinations_)) {
            for (auto& stream : streams) {
                if (stream.client_id == request.client_id) {
                    stream.endpoint.video_uri =
                        rtp_h264_uri(request.destination_host, request.port);
                }
            }
        }
    }
    if (capture_.audio) {
        try {
            restart_audio();
            for (const auto& request : audio_requests_from_media_destinations(plan_, destinations_)) {
                for (auto& stream : streams) {
                    if (stream.client_id == request.client_id) {
                        stream.endpoint.audio_uri =
                            rtp_opus_uri(request.destination_host, request.port);
                    }
                }
            }
        } catch (const std::exception& error) {
            capture_.audio = false;
            std::cerr << "Warning: audio streaming disabled: " << error.what() << '\n';
        }
    }
}

MediaEndpoint WindowsMediaServer::add_client(
    ClientId client_id,
    const std::string& destination_host,
    std::size_t /*media_index*/,
    bool wants_video,
    bool wants_audio) {
    destinations_.push_back(HostMediaDestination{client_id, destination_host});
    MediaEndpoint endpoint;
    const auto index = destinations_.size() - 1;
    if (wants_video && capture_.video && plan_.video) {
        const auto request = video_request_for_destination(plan_, destinations_.back(), index);
        endpoint.video_uri = rtp_h264_uri(request.destination_host, request.port);
        restart_video();
    }
    if (wants_audio && capture_.audio && plan_.audio) {
        const auto request = audio_request_for_destination(plan_, destinations_.back(), index);
        endpoint.audio_uri = rtp_opus_uri(request.destination_host, request.port);
        restart_audio();
    }
    return endpoint;
}

void WindowsMediaServer::remove_client(ClientId client_id) {
    destinations_.erase(
        std::remove_if(
            destinations_.begin(),
            destinations_.end(),
            [client_id](const HostMediaDestination& destination) {
                return destination.client_id == client_id;
            }),
        destinations_.end());
    restart_video();
    restart_audio();
}

bool WindowsMediaServer::reconfigure_client_video(ClientId, const VideoEncodeSettings&) {
    return false;
}

void WindowsMediaServer::stop() {
    if (video_running_) {
        video_process_.stop();
        video_running_ = false;
    }
    if (audio_running_) {
        audio_process_.stop();
        audio_running_ = false;
    }
}

} // namespace archstreamer

#endif // _WIN32
