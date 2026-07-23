#include "client/gstreamer_media_receiver.hpp"

#include "common/addresses.hpp"
#include "common/platform/paths.hpp"

#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
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
    // Direct3D11 decoders emit D3D11 memory; pair them with d3d11videosink and skip system videoconvert.
    bool d3d11_zero_copy = false;
};

H264DecoderChoice choose_h264_decoder() {
    if (!gst_inspect_available()) {
#ifdef _WIN32
        throw std::runtime_error(
            "gst-inspect-1.0 not found on PATH. Install the GStreamer MSVC runtime "
            "(https://gstreamer.freedesktop.org/download/) and add its bin directory to PATH, "
            "then restart ArchStreamer.");
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
#ifdef _WIN32
    throw std::runtime_error(
        "no H.264 decoder found (need d3d11h264dec, mfh264dec, avdec_h264, or openh264dec). "
        "Install the full GStreamer MSVC runtime (MSVC 64-bit), not just a minimal package, "
        "and ensure its bin directory is on PATH.");
#else
    throw std::runtime_error(
        "no H.264 decoder found (need avdec_h264 or openh264dec). "
        "On Flatpak, install org.freedesktop.Platform.openh264 matching the runtime.");
#endif
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

std::string to_lower_copy(std::string value) {
    for (char& ch : value) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return value;
}

bool contains_ci(const std::string& haystack, const char* needle) {
    return to_lower_copy(haystack).find(to_lower_copy(std::string{needle})) != std::string::npos;
}

bool looks_like_controller_audio_device(const std::string& name) {
    return contains_ci(name, "wireless controller") ||
        contains_ci(name, "dualsense") ||
        contains_ci(name, "dualshock") ||
        contains_ci(name, "playstation") ||
        contains_ci(name, "xbox controller") ||
        contains_ci(name, "gamepad") ||
        contains_ci(name, "hands-free");
}

struct WasapiSinkDevice {
    std::string id;
    std::string name;
    std::string enumerator;
    bool is_default = false;
};

std::string capture_command_output(const std::string& command) {
#ifdef _WIN32
    FILE* pipe = _popen(command.c_str(), "r");
#else
    FILE* pipe = popen(command.c_str(), "r");
#endif
    if (pipe == nullptr) {
        return {};
    }
    std::string output;
    char buffer[512];
    while (std::fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        output += buffer;
    }
#ifdef _WIN32
    _pclose(pipe);
#else
    pclose(pipe);
#endif
    return output;
}

std::string trim_copy(std::string value) {
    while (!value.empty() && (value.back() == '\r' || value.back() == '\n' || value.back() == ' ' || value.back() == '\t')) {
        value.pop_back();
    }
    std::size_t start = 0;
    while (start < value.size() && (value[start] == ' ' || value[start] == '\t')) {
        ++start;
    }
    return value.substr(start);
}

std::vector<WasapiSinkDevice> list_wasapi2_sink_devices() {
    std::vector<WasapiSinkDevice> devices;
#ifdef _WIN32
    if (!gst_element_available("wasapi2sink")) {
        return devices;
    }
    const auto output = capture_command_output("gst-device-monitor-1.0.exe Audio/Sink");
    WasapiSinkDevice current;
    bool in_device = false;
    auto flush = [&] {
        if (in_device && !current.id.empty()) {
            devices.push_back(current);
        }
        current = {};
        in_device = false;
    };
    std::size_t line_start = 0;
    while (line_start <= output.size()) {
        const auto line_end = output.find('\n', line_start);
        std::string line = output.substr(
            line_start,
            line_end == std::string::npos ? std::string::npos : line_end - line_start);
        line_start = line_end == std::string::npos ? output.size() + 1 : line_end + 1;
        line = trim_copy(std::move(line));
        if (line == "Device found:") {
            flush();
            in_device = true;
            continue;
        }
        if (!in_device) {
            continue;
        }
        constexpr std::string_view name_key = "name  : ";
        constexpr std::string_view id_key = "device.id = ";
        constexpr std::string_view default_key = "device.default = ";
        constexpr std::string_view enumerator_key = "device.enumerator-name = ";
        constexpr std::string_view actual_name_key = "device.actual-name = ";
        if (line.rfind(name_key, 0) == 0) {
            current.name = line.substr(name_key.size());
        } else if (line.rfind(id_key, 0) == 0) {
            current.id = line.substr(id_key.size());
        } else if (line.rfind(default_key, 0) == 0) {
            current.is_default = line.find("true") != std::string::npos;
        } else if (line.rfind(enumerator_key, 0) == 0) {
            current.enumerator = line.substr(enumerator_key.size());
        } else if (line.rfind(actual_name_key, 0) == 0) {
            // Default virtual device often points at the controller; use the real name.
            current.name = line.substr(actual_name_key.size());
        }
    }
    flush();
#endif
    return devices;
}

std::optional<WasapiSinkDevice> choose_preferred_wasapi2_device() {
    const auto devices = list_wasapi2_sink_devices();
    auto score = [](const WasapiSinkDevice& device) {
        if (device.id.empty() || looks_like_controller_audio_device(device.name)) {
            return -1000;
        }
        // Skip the virtual "Default Audio Render Device" entry; pick a concrete endpoint.
        if (contains_ci(device.name, "default audio render device")) {
            return -500;
        }
        int value = 0;
        if (contains_ci(device.enumerator, "HDAUDIO") || contains_ci(device.name, "Realtek")) {
            value += 100;
        }
        if (contains_ci(device.name, "Speakers")) {
            value += 20;
        }
        if (contains_ci(device.enumerator, "USB")) {
            value -= 10;
        }
        if (device.is_default) {
            value += 5;
        }
        return value;
    };

    const WasapiSinkDevice* best = nullptr;
    int best_score = 0;
    for (const auto& device : devices) {
        const int value = score(device);
        if (value > best_score) {
            best_score = value;
            best = &device;
        }
    }
    if (best == nullptr) {
        return std::nullopt;
    }
    return *best;
}

// Returns gst-launch argument tokens for the sink element (may include properties).
struct AudioSinkChoice {
    std::vector<std::string> args;
    std::string description = "autoaudiosink";
};

AudioSinkChoice choose_audio_sink() {
#ifdef _WIN32
    // DualSense/etc. often become Windows' default render device ("Speakers (Wireless
    // Controller)"). Avoid the default endpoint and pin wasapi2 to a real laptop sink.
    if (const auto device = choose_preferred_wasapi2_device(); device.has_value()) {
        return {
            {
                "wasapi2sink",
                "device=" + device->id,
                "sync=false",
            },
            "wasapi2sink:" + device->name,
        };
    }
    if (gst_element_available("wasapisink")) {
        return {{"wasapisink", "role=multimedia", "sync=false"}, "wasapisink role=multimedia"};
    }
    if (gst_element_available("directsoundsink")) {
        return {{"directsoundsink", "sync=false"}, "directsoundsink"};
    }
#endif
    return {{"autoaudiosink", "sync=false"}, "autoaudiosink"};
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
        std::string detail;
        const auto log_path = video_stderr_log_path();
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
}

std::vector<std::string> build_video_pipeline_args(
    std::uint16_t port,
    const H264DecoderChoice& decoder,
    const VideoSinkChoice& sink) {
#ifdef _WIN32
    // Must include .exe: CreateProcess treats "gst-launch-1.0" as extension ".0".
    const char* gst_launch = "gst-launch-1.0.exe";
#else
    const char* gst_launch = "gst-launch-1.0";
#endif
    std::vector<std::string> args{
        gst_launch,
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
    // h264parse lives in plugins-bad. Prefer it when present; many decoders (avdec_h264)
    // still work with depay output alone.
    if (gst_element_available("h264parse")) {
        args.push_back("h264parse");
        args.push_back("!");
    }
    args.push_back(decoder.element);
    args.push_back("!");

    if (decoder.d3d11_zero_copy && sink.kind == VideoSinkKind::D3D11) {
        // Keep frames in D3D11 memory for the Windows HW path.
        args.push_back(sink.element);
        args.push_back("sync=false");
        return args;
    }

    if (decoder.d3d11_zero_copy) {
        args.push_back("d3d11download");
        args.push_back("!");
    }

    args.insert(args.end(), {
        "videoconvert",
        "!",
        // Prints when decoded frames flow (shows up in the stderr log).
        "identity",
        "silent=false",
        "!",
        sink.element,
        "sync=false",
    });
    return args;
}

} // namespace

void GStreamerMediaReceiver::connect(const MediaEndpoint& endpoint) {
    video_pipeline_info_.clear();
    audio_pipeline_info_.clear();
    if (!endpoint.video_uri.empty()) {
        const auto port = video_port_from_endpoint(endpoint);
        const auto decoder = choose_h264_decoder();
        const auto sink = choose_video_sink(decoder.d3d11_zero_copy);
        const auto log_path = video_stderr_log_path();
        video_pipeline_info_ = std::string("decoder=") + decoder.element + " sink=" + sink.element +
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
            build_video_pipeline_args(port, decoder, sink),
            environment,
            unset,
            log_path.string());
        ensure_process_stayed_up(video_process_, "Video");
    }

    if (!endpoint.audio_uri.empty()) {
        const auto port = audio_port_from_endpoint(endpoint);
#ifdef _WIN32
        const char* gst_launch = "gst-launch-1.0.exe";
#else
        const char* gst_launch = "gst-launch-1.0";
#endif
        std::vector<std::string> audio_args{
            gst_launch,
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
        };
        const auto sink = choose_audio_sink();
        audio_pipeline_info_ = sink.description;
        audio_args.insert(audio_args.end(), sink.args.begin(), sink.args.end());
        audio_process_.start(audio_args);
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

const std::string& GStreamerMediaReceiver::audio_pipeline_info() const {
    return audio_pipeline_info_;
}

bool GStreamerMediaReceiver::video_frames_seen() const {
    return decoded_frame_count() > 0;
}

std::uint64_t GStreamerMediaReceiver::decoded_frame_count() const {
    const auto marker = video_pipeline_info_.find("log=");
    if (marker == std::string::npos) {
        return 0;
    }
    const auto path = video_pipeline_info_.substr(marker + 4);
    std::ifstream in(path);
    if (!in) {
        return 0;
    }
    std::uint64_t count = 0;
    std::string line;
    while (std::getline(in, line)) {
        // identity silent=false emits lines containing "identity" / buffer info.
        if (line.find("identity") != std::string::npos || line.find("buffer") != std::string::npos) {
            ++count;
        }
    }
    return count;
}

} // namespace archstreamer
