#include "client/gstreamer_media_receiver.hpp"

#include "common/addresses.hpp"
#include "common/platform/paths.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace archstreamer {
namespace {

bool gst_element_available(const char* element) {
#ifdef _WIN32
    const auto command = std::string("gst-inspect-1.0 ") + element + " >NUL 2>&1";
#else
    const auto command = std::string("gst-inspect-1.0 ") + element + " >/dev/null 2>&1";
#endif
    return std::system(command.c_str()) == 0;
}

const char* choose_h264_decoder() {
    if (gst_element_available("avdec_h264")) {
        return "avdec_h264";
    }
    if (gst_element_available("openh264dec")) {
        return "openh264dec";
    }
    throw std::runtime_error(
        "no H.264 decoder found (need avdec_h264 or openh264dec). "
        "On Flatpak, install org.freedesktop.Platform.openh264 matching the runtime.");
}

enum class VideoSinkKind { X11, Wayland, Other };

struct VideoSinkChoice {
    const char* element = "autovideosink";
    VideoSinkKind kind = VideoSinkKind::Other;
};

VideoSinkChoice choose_video_sink() {
#ifdef _WIN32
    if (gst_element_available("d3d11videosink")) {
        return {"d3d11videosink", VideoSinkKind::Other};
    }
    return {"autovideosink", VideoSinkKind::Other};
#else
    // Prefer X11 when DISPLAY exists (Flatpak fallback-x11 / XWayland).
    if (std::getenv("DISPLAY") != nullptr) {
        if (gst_element_available("xvimagesink")) {
            return {"xvimagesink", VideoSinkKind::X11};
        }
        if (gst_element_available("ximagesink")) {
            return {"ximagesink", VideoSinkKind::X11};
        }
    }
    if (std::getenv("WAYLAND_DISPLAY") != nullptr && gst_element_available("waylandsink")) {
        return {"waylandsink", VideoSinkKind::Wayland};
    }
    if (gst_element_available("glimagesink")) {
        return {"glimagesink", VideoSinkKind::Other};
    }
    return {"autovideosink", VideoSinkKind::Other};
#endif
}

std::filesystem::path video_stderr_log_path() {
    auto root = archstreamer_cache_directory();
    if (root.empty()) {
        root = (std::filesystem::temp_directory_path() / "archstreamer").string();
    }
    std::error_code ec;
    std::filesystem::create_directories(root, ec);
    return std::filesystem::path{root} / "gst-video-receiver.log";
}

void ensure_process_stayed_up(const ChildProcess& process, const char* label) {
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    if (!process.running()) {
        throw std::runtime_error(
            std::string(label) +
            " GStreamer pipeline exited immediately. "
            "Check gst-launch-1.0 plugins (H.264 decode / Opus) and UDP media ports.");
    }
}

} // namespace

void GStreamerMediaReceiver::connect(const MediaEndpoint& endpoint) {
    video_pipeline_info_.clear();
    if (!endpoint.video_uri.empty()) {
        const auto port = video_port_from_endpoint(endpoint);
        const char* decoder = choose_h264_decoder();
        const auto sink = choose_video_sink();
        const auto log_path = video_stderr_log_path();
        video_pipeline_info_ = std::string("decoder=") + decoder + " sink=" + sink.element +
            " log=" + log_path.string();

        auto environment = std::vector<std::pair<std::string, std::string>>{};
        auto unset = std::vector<std::string>{};
        // Only strip Wayland when driving an X11 sink; otherwise the sink has no display.
        if (sink.kind == VideoSinkKind::X11) {
            environment.push_back({"GDK_BACKEND", "x11"});
            environment.push_back({"GST_GL_WINDOW", "x11"});
            environment.push_back({"GST_GL_PLATFORM", "glx"});
            unset.push_back("WAYLAND_DISPLAY");
            unset.push_back("WAYLAND_SOCKET");
        }

        video_process_.start(
            {
                "gst-launch-1.0",
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
                "h264parse",
                "!",
                decoder,
                "!",
                "videoconvert",
                "!",
                // Prints when decoded frames flow (shows up in the stderr log).
                "identity",
                "silent=false",
                "!",
                sink.element,
                "sync=false",
            },
            environment,
            unset,
            log_path.string());
        ensure_process_stayed_up(video_process_, "Video");
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
            "sync=false",
        });
        ensure_process_stayed_up(audio_process_, "Audio");
    }
}

void GStreamerMediaReceiver::disconnect() {
    audio_process_.stop();
    video_process_.stop();
}

bool GStreamerMediaReceiver::video_running() const {
    return video_process_.running();
}

bool GStreamerMediaReceiver::audio_running() const {
    return audio_process_.running();
}

const std::string& GStreamerMediaReceiver::video_pipeline_info() const {
    return video_pipeline_info_;
}

bool GStreamerMediaReceiver::video_frames_seen() const {
    const auto marker = video_pipeline_info_.find("log=");
    if (marker == std::string::npos) {
        return false;
    }
    const auto path = video_pipeline_info_.substr(marker + 4);
    std::ifstream in(path);
    if (!in) {
        return false;
    }
    std::string line;
    while (std::getline(in, line)) {
        // identity silent=false emits lines containing "identity" / buffer info.
        if (line.find("identity") != std::string::npos || line.find("buffer") != std::string::npos) {
            return true;
        }
    }
    return false;
}

} // namespace archstreamer
