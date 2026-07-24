#include "client/gstreamer_media_pipeline.hpp"

#include "client/gstreamer_probe.hpp"
#include "common/platform/paths.hpp"

#include <chrono>
#include <fstream>
#include <stdexcept>
#include <thread>

namespace archstreamer {
namespace {

std::filesystem::path cache_log_path(const char* filename) {
    auto root = archstreamer_cache_directory();
    if (root.empty()) {
        root = (std::filesystem::temp_directory_path() / "archstreamer").string();
    }
    std::error_code ec;
    std::filesystem::create_directories(root, ec);
    return std::filesystem::path{root} / filename;
}

} // namespace

std::filesystem::path gst_video_receiver_log_path() {
    return cache_log_path("gst-video-receiver.log");
}

std::filesystem::path gst_synced_receiver_log_path() {
    return cache_log_path("gst-synced-media-receiver.log");
}

void ensure_gst_child_stayed_up(
    const ChildProcess& process,
    const char* label,
    const std::filesystem::path& log_path) {
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    if (process.running()) {
        return;
    }
    std::string detail;
    std::ifstream log(log_path);
    if (log) {
        std::string line;
        while (std::getline(log, line)) {
            if (!detail.empty()) {
                detail.push_back(' ');
            }
            detail += line;
            if (detail.size() > 240) {
                detail.resize(240);
                detail += "...";
                break;
            }
        }
    }
    throw std::runtime_error(
        std::string(label) +
        " GStreamer pipeline exited immediately. "
        "Check gst-launch-1.0 plugins (H.264 decode / Opus) and UDP media ports." +
        (detail.empty() ? "" : (" Log: " + detail)));
}

std::vector<std::string> gst_h264_rtp_source_args(std::uint16_t port) {
    return {
        "udpsrc",
        "port=" + std::to_string(port),
        "buffer-size=2097152",
        "caps=application/x-rtp,media=video,encoding-name=H264,payload=96,clock-rate=90000",
        "!",
        "rtpjitterbuffer",
        "latency=80",
        "!",
        "rtph264depay",
        "!",
    };
}

std::vector<std::string> gst_opus_rtp_decode_args(std::uint16_t port, int jitter_latency_ms) {
    return {
        "udpsrc",
        "port=" + std::to_string(port),
        "caps=application/x-rtp,media=audio,encoding-name=OPUS,payload=97,clock-rate=48000,encoding-params=2",
        "!",
        "rtpjitterbuffer",
        "latency=" + std::to_string(jitter_latency_ms),
        "!",
        "rtpopusdepay",
        "!",
        "opusdec",
        "!",
        "audioconvert",
        "!",
        "audioresample",
        "!",
    };
}

void gst_append_h264parse_if_available(std::vector<std::string>& args) {
    if (!gst_element_available("h264parse")) {
        return;
    }
    args.push_back("h264parse");
    args.push_back("!");
}

void gst_append_progress_video_sink(
    std::vector<std::string>& args,
    const GstVideoSinkChoice& sink,
    bool sync) {
    args.insert(args.end(), {
        "videoconvert",
        "!",
        "progressreport",
        "update-freq=1",
        "!",
        sink.element,
        sync ? "sync=true" : "sync=false",
    });
}

} // namespace archstreamer
