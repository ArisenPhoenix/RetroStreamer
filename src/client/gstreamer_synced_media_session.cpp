#include "client/gstreamer_synced_media_session.hpp"

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

bool gst_inspect_available() {
#ifdef _WIN32
    return std::system("gst-inspect-1.0 --version >NUL 2>&1") == 0;
#else
    return std::system("gst-inspect-1.0 --version >/dev/null 2>&1") == 0;
#endif
}

struct H264DecoderChoice {
    const char* element = nullptr;
    bool d3d11_zero_copy = false;
};

H264DecoderChoice choose_h264_decoder() {
    if (!gst_inspect_available()) {
#ifdef _WIN32
        throw std::runtime_error(
            "gst-inspect-1.0 not found on PATH. Install the GStreamer MSVC runtime "
            "and add its bin directory to PATH.");
#else
        throw std::runtime_error(
            "gst-inspect-1.0 not found on PATH. Install GStreamer tools and ensure they are on PATH.");
#endif
    }
#ifdef _WIN32
    if (gst_element_available("d3d11h264dec")) {
        return {"d3d11h264dec", true};
    }
    if (gst_element_available("mfh264dec")) {
        return {"mfh264dec", false};
    }
#endif
    if (gst_element_available("avdec_h264")) {
        return {"avdec_h264", false};
    }
    if (gst_element_available("openh264dec")) {
        return {"openh264dec", false};
    }
    throw std::runtime_error("no H.264 decoder found for synced media session");
}

enum class VideoSinkKind { X11, Wayland, D3D11, Other };

struct VideoSinkChoice {
    const char* element = "autovideosink";
    VideoSinkKind kind = VideoSinkKind::Other;
};

VideoSinkChoice choose_video_sink(bool prefer_d3d11) {
#ifdef _WIN32
    if (prefer_d3d11 && gst_element_available("d3d11videosink")) {
        return {"d3d11videosink", VideoSinkKind::D3D11};
    }
    if (gst_element_available("d3d11videosink")) {
        return {"d3d11videosink", VideoSinkKind::D3D11};
    }
    return {"autovideosink", VideoSinkKind::Other};
#else
    (void)prefer_d3d11;
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

struct AudioSinkChoice {
    std::vector<std::string> args;
    std::string description = "autoaudiosink";
};

AudioSinkChoice choose_audio_sink_synced() {
#ifdef _WIN32
    if (gst_element_available("wasapisink")) {
        return {{"wasapisink", "role=multimedia", "sync=true"}, "wasapisink role=multimedia sync=true"};
    }
    if (gst_element_available("directsoundsink")) {
        return {{"directsoundsink", "sync=true"}, "directsoundsink sync=true"};
    }
#endif
    return {{"autoaudiosink", "sync=true"}, "autoaudiosink sync=true"};
}

const char* gst_launch_bin() {
#ifdef _WIN32
    return "gst-launch-1.0.exe";
#else
    return "gst-launch-1.0";
#endif
}

std::filesystem::path synced_stderr_log_path() {
    auto root = archstreamer_cache_directory();
    if (root.empty()) {
        root = (std::filesystem::temp_directory_path() / "archstreamer").string();
    }
    std::error_code ec;
    std::filesystem::create_directories(root, ec);
    return std::filesystem::path{root} / "gst-synced-media-receiver.log";
}

void append_video_branch(
    std::vector<std::string>& args,
    std::uint16_t port,
    const H264DecoderChoice& decoder,
    const VideoSinkChoice& sink) {
    args.insert(args.end(), {
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
    });
    if (gst_element_available("h264parse")) {
        args.push_back("h264parse");
        args.push_back("!");
    }
    args.push_back(decoder.element);
    args.push_back("!");

    if (decoder.d3d11_zero_copy && sink.kind == VideoSinkKind::D3D11) {
        args.push_back(sink.element);
        args.push_back("sync=true");
        return;
    }
    if (decoder.d3d11_zero_copy) {
        args.push_back("d3d11download");
        args.push_back("!");
    }
    args.insert(args.end(), {
        "videoconvert",
        "!",
        "identity",
        "silent=false",
        "!",
        sink.element,
        "sync=true",
    });
}

void append_audio_branch(
    std::vector<std::string>& args,
    std::uint16_t port,
    const AudioSinkChoice& sink) {
    args.insert(args.end(), {
        "udpsrc",
        "port=" + std::to_string(port),
        "caps=application/x-rtp,media=audio,encoding-name=OPUS,payload=97,clock-rate=48000,encoding-params=2",
        "!",
        "rtpjitterbuffer",
        // Match video jitter latency so the shared clock has less skew to absorb.
        "latency=80",
        "!",
        "rtpopusdepay",
        "!",
        "opusdec",
        "!",
        "audioconvert",
        "!",
        "audioresample",
        "!",
    });
    args.insert(args.end(), sink.args.begin(), sink.args.end());
}

} // namespace

GStreamerSyncedMediaSession::GStreamerSyncedMediaSession() {
    video_.session_ = this;
    audio_.session_ = this;
}

GStreamerSyncedMediaSession::~GStreamerSyncedMediaSession() {
    disconnect();
}

bool GStreamerSyncedMediaSession::VideoBranch::running() const {
    return enabled_ && session_ != nullptr && session_->running();
}

bool GStreamerSyncedMediaSession::AudioBranch::running() const {
    return enabled_ && session_ != nullptr && session_->running();
}

bool GStreamerSyncedMediaSession::running() const {
    return process_.running();
}

void GStreamerSyncedMediaSession::connect(const MediaEndpoint& endpoint) {
    disconnect();

    const bool want_video = !endpoint.video_uri.empty();
    const bool want_audio = !endpoint.audio_uri.empty();
    if (!want_video && !want_audio) {
        return;
    }

    auto args = std::vector<std::string>{gst_launch_bin(), "-q"};
    auto environment = std::vector<std::pair<std::string, std::string>>{};
    auto unset = std::vector<std::string>{};
    stderr_log_path_ = synced_stderr_log_path().string();

    if (want_video) {
        const auto decoder = choose_h264_decoder();
        const auto sink = choose_video_sink(decoder.d3d11_zero_copy);
        video_.enabled_ = true;
        video_.pipeline_info_ = std::string("synced decoder=") + decoder.element +
            " sink=" + sink.element + " sync=true log=" + stderr_log_path_;
        append_video_branch(args, video_port_from_endpoint(endpoint), decoder, sink);

        if (sink.kind == VideoSinkKind::X11) {
            environment.push_back({"GDK_BACKEND", "x11"});
            environment.push_back({"GST_GL_WINDOW", "x11"});
            environment.push_back({"GST_GL_PLATFORM", "glx"});
            unset.push_back("WAYLAND_DISPLAY");
            unset.push_back("WAYLAND_SOCKET");
        }
    }

    if (want_audio) {
        const auto sink = choose_audio_sink_synced();
        audio_.enabled_ = true;
        audio_.pipeline_info_ = "synced " + sink.description;
        append_audio_branch(args, audio_port_from_endpoint(endpoint), sink);
    }

    process_.start(std::move(args), environment, unset, stderr_log_path_);

    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    if (!process_.running()) {
        std::string detail;
        if (!stderr_log_path_.empty()) {
            std::ifstream log(stderr_log_path_);
            std::string line;
            while (log && std::getline(log, line)) {
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
        disconnect();
        throw std::runtime_error(
            "Synced media pipeline exited immediately. "
            "Check gst-launch-1.0 plugins (H.264 / Opus) and UDP media ports." +
            (detail.empty() ? "" : (" Log: " + detail)));
    }
}

void GStreamerSyncedMediaSession::disconnect() {
    process_.stop();
    video_.enabled_ = false;
    audio_.enabled_ = false;
    video_.pipeline_info_.clear();
    audio_.pipeline_info_.clear();
    stderr_log_path_.clear();
}

std::uint64_t GStreamerSyncedMediaSession::decoded_frame_count() const {
    if (stderr_log_path_.empty()) {
        return 0;
    }
    std::ifstream in(stderr_log_path_);
    if (!in) {
        return 0;
    }
    std::uint64_t count = 0;
    std::string line;
    while (std::getline(in, line)) {
        if (line.find("identity") != std::string::npos || line.find("buffer") != std::string::npos) {
            ++count;
        }
    }
    return count;
}

bool GStreamerSyncedMediaSession::video_frames_seen() const {
    return decoded_frame_count() > 0;
}

void GStreamerSyncedMediaReceiver::connect(const MediaEndpoint& endpoint) {
    session_.connect(endpoint);
}

void GStreamerSyncedMediaReceiver::disconnect() {
    session_.disconnect();
}

bool GStreamerSyncedMediaReceiver::video_running() const {
    return session_.video().running();
}

bool GStreamerSyncedMediaReceiver::audio_running() const {
    return session_.audio().running();
}

bool GStreamerSyncedMediaReceiver::video_frames_seen() const {
    return session_.video_frames_seen();
}

std::uint64_t GStreamerSyncedMediaReceiver::decoded_frame_count() const {
    return session_.decoded_frame_count();
}

const std::string& GStreamerSyncedMediaReceiver::video_pipeline_info() const {
    return session_.video().pipeline_info();
}

const std::string& GStreamerSyncedMediaReceiver::audio_pipeline_info() const {
    return session_.audio().pipeline_info();
}

} // namespace archstreamer
