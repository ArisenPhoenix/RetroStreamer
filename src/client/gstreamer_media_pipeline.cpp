#include "client/gstreamer_media_pipeline.hpp"

#include "client/gstreamer_probe.hpp"
#include "common/platform/paths.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#ifndef _WIN32
#include <X11/Xlib.h>
#endif

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

// On small displays (SPICE/QXL VMs) letterbox into ~85% of the screen so a 1080p
// stream does not open larger than the desktop. On normal/large displays skip the
// forced canvas — sinks open at stream size and the user can resize; forcing a
// fixed WxH here made glimagesink/gtksink show a tiny centered picture.
std::optional<std::pair<int, int>> detect_view_max_size() {
#ifdef _WIN32
    return std::nullopt;
#else
    Display* display = XOpenDisplay(nullptr);
    if (display == nullptr) {
        return std::nullopt;
    }
    const int screen = DefaultScreen(display);
    const int width = DisplayWidth(display, screen);
    const int height = DisplayHeight(display, screen);
    XCloseDisplay(display);
    if (width < 320 || height < 240) {
        return std::nullopt;
    }
    // Only constrain when the desktop is smaller than a 1080p stream.
    if (width >= 1600 && height >= 900) {
        return std::nullopt;
    }
    const int out_w = std::max(640, width * 85 / 100);
    const int out_h = std::max(360, height * 85 / 100);
    return std::pair<int, int>{out_w, out_h};
#endif
}

// Flatpak cannot set SO_RCVBUF above net.core.rmem_max without CAP_NET_ADMIN.
// Default 0 = kernel default (safe everywhere). scripts/grant-flatpak-udp-buffers.sh
// raises rmem_* and sets ARCHSTREAMER_UDP_RCVBUF for a larger Wi‑Fi-friendly buffer.
int udp_receive_buffer_bytes() {
    if (const char* env = std::getenv("ARCHSTREAMER_UDP_RCVBUF");
        env != nullptr && env[0] != '\0') {
        try {
            const auto value = std::stoll(env);
            if (value >= 0 && value <= 64LL * 1024 * 1024) {
                return static_cast<int>(value);
            }
        } catch (const std::exception&) {
        }
    }
    return 0;
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
    // Default buffer-size=0 (kernel default) is safe in Flatpak. After
    // scripts/grant-flatpak-udp-buffers.sh raises rmem_max and sets
    // ARCHSTREAMER_UDP_RCVBUF, use that larger SO_RCVBUF for Wi‑Fi IDR bursts.
    return {
        "udpsrc",
        "port=" + std::to_string(port),
        "buffer-size=" + std::to_string(udp_receive_buffer_bytes()),
        "caps=application/x-rtp,media=video,encoding-name=H264,payload=96,clock-rate=90000",
        "!",
        "rtpjitterbuffer",
        // Slightly roomier than LAN-ethernet defaults — Bazzite clients are often Wi‑Fi.
        "latency=150",
        "drop-on-latency=false",
        "!",
        "rtph264depay",
        "!",
    };
}

std::vector<std::string> gst_opus_rtp_decode_args(std::uint16_t port, int jitter_latency_ms) {
    // Default ~150 ms; Windows/Wi‑Fi clients benefit from a roomier buffer.
    int latency = jitter_latency_ms > 0 ? jitter_latency_ms : 150;
#ifdef _WIN32
    if (jitter_latency_ms <= 0) {
        latency = 250;
    }
#endif
    return {
        "udpsrc",
        "port=" + std::to_string(port),
        "caps=application/x-rtp,media=audio,encoding-name=OPUS,payload=97,clock-rate=48000,encoding-params=2",
        "!",
        "rtpjitterbuffer",
        "latency=" + std::to_string(latency),
        // Match video: late Opus packets are better late than dropped (Wi‑Fi / WASAPI hiccups).
        "drop-on-latency=false",
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
    args.push_back("videoconvert");
    args.push_back("!");

    // Letterbox into a canvas that fits the local display so 1080p streams do not
    // open larger than a small VM/SPICE desktop (and stay centered via add-borders).
    if (const auto max = detect_view_max_size(); max.has_value()) {
        args.insert(args.end(), {
            "videoscale",
            "method=0",
            "add-borders=true",
            "!",
            "video/x-raw,width=" + std::to_string(max->first) +
                ",height=" + std::to_string(max->second),
            "!",
        });
    }

    args.insert(args.end(), {
        "progressreport",
        "update-freq=1",
        "!",
        sink.element,
        sync ? "sync=true" : "sync=false",
        // Never drop late decoded frames — static intro scene-cuts are infrequent and
        // must still paint even if jitter pushes them slightly past the clock.
        "qos=false",
        "max-lateness=-1",
    });
    // Keep letterboxing when the user resizes a GTK/X11 window.
    if (std::strcmp(sink.element, "gtksink") == 0 ||
        std::strcmp(sink.element, "gtkglsink") == 0 ||
        std::strcmp(sink.element, "ximagesink") == 0 ||
        std::strcmp(sink.element, "xvimagesink") == 0 ||
        std::strcmp(sink.element, "glimagesink") == 0) {
        args.push_back("force-aspect-ratio=true");
    }
    // Window close → EOS → gst-launch exits → ClientApp stops audio + session.
    // gtksink lacks this; that is why it is deprioritized when an X11 sink works.
    if (std::strcmp(sink.element, "ximagesink") == 0 ||
        std::strcmp(sink.element, "xvimagesink") == 0 ||
        std::strcmp(sink.element, "glimagesink") == 0) {
        args.push_back("handle-events=true");
    }
    if (std::strcmp(sink.element, "ximagesink") == 0 ||
        std::strcmp(sink.element, "xvimagesink") == 0) {
        // Repaint on expose so a covered/uncovered window still shows the last frame.
        args.push_back("handle-expose=true");
    }
}

} // namespace archstreamer
