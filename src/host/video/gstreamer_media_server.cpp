#include "common/addresses.hpp"
#include "common/platform/paths.hpp"
#include "common/platform/process_utils.hpp"
#include "host/hardware/gpu_select.hpp"
#include "host/video/gstreamer_media_server.hpp"
#include "host/video/rtp_frame_pace_debug.hpp"
#if defined(ARCHSTREAMER_HAS_GST_LIBS)
#include "host/video/gst/gst_video_engine.hpp"
#include "host/video/gst/gst_video_pipeline.hpp"
#include "host/video/gst/gst_video_worker_client.hpp"
#endif

#include <array>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>

#include <unistd.h>

namespace archstreamer {
namespace {

std::string getenv_string(const char* key) {
    const char* value = std::getenv(key);
    return value != nullptr ? std::string(value) : std::string{};
}

bool getenv_bool(const char* key) {
    auto value = getenv_string(key);
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value == "1" || value == "true" || value == "yes" || value == "on";
}

std::string pipewire_user_runtime_dir() {
    const auto env_runtime = getenv_string("XDG_RUNTIME_DIR");
    if (!env_runtime.empty()) {
        std::error_code ec;
        if (std::filesystem::exists(std::filesystem::path(env_runtime) / "pipewire-0", ec) && !ec) {
            return env_runtime;
        }
    }
    const auto user_runtime =
        std::filesystem::path("/run/user") / std::to_string(static_cast<long long>(geteuid()));
    return user_runtime.string();
}

void configure_managed_pipewire_environment() {
#if defined(__unix__)
    const auto runtime = pipewire_user_runtime_dir();
    if (runtime.empty()) {
        return;
    }
    const auto original_pipewire_runtime = getenv_string("PIPEWIRE_RUNTIME_DIR");
    if (getenv_string("XDG_RUNTIME_DIR").empty()) {
        setenv("XDG_RUNTIME_DIR", runtime.c_str(), 1);
    }
    std::cout
        << "Managed GStreamer PipeWire runtime: XDG_RUNTIME_DIR="
        << getenv_string("XDG_RUNTIME_DIR")
        << " PIPEWIRE_RUNTIME_DIR="
        << (original_pipewire_runtime.empty() ? "<unset>" : original_pipewire_runtime)
        << '\n';
#endif
}

// gst_parse_launch() description strings: only double quotes are stripped.
// Single quotes stay in the property (display-name=':99' → "':99'"), which
// makes ximagesrc's XOpenDisplay fail with "Could not open X display".
std::string gst_parse_quote(std::string_view value) {
    std::string quoted = "\"";
    for (const char ch : value) {
        if (ch == '\\' || ch == '"') {
            quoted.push_back('\\');
        }
        quoted.push_back(ch);
    }
    quoted.push_back('"');
    return quoted;
}

std::string gst_parse_property(std::string_view key, std::string_view value) {
    return std::string(key) + "=" + gst_parse_quote(value);
}

// gst-launch-1.0 argv: each key=value is already one exec token, then the
// tool joins tokens with spaces and calls gst_parse_launch. Leave simple
// values bare (display-name=:99). Quote only when the joined description
// would split the value. Never wrap with single quotes.
std::string gst_argv_property(std::string_view key, std::string_view value) {
    const bool needs_quotes = value.empty() ||
        std::any_of(value.begin(), value.end(), [](char ch) {
            return ch == ' ' || ch == '\t' || ch == '!' || ch == '"' || ch == '\\';
        });
    if (needs_quotes) {
        return std::string(key) + "=" + gst_parse_quote(value);
    }
    return std::string(key) + "=" + std::string(value);
}

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

/** TEMP: frame pacing debug — optional loopback RTP tee. */
std::vector<std::pair<std::string, std::uint16_t>> multiudp_clients_with_pace_tee(
    std::vector<std::pair<std::string, std::uint16_t>> clients,
    std::uint16_t encode_port) {
    if (auto tee = rtp_frame_pace_debug::ensure_tee(encode_port)) {
        clients.push_back(*tee);
    }
    return clients;
}

bool gst_element_available(const char* element) {
#if defined(ARCHSTREAMER_HAS_GST_LIBS)
    return gst_video_element_available(element);
#else
    (void)element;
    return false;
#endif
}

enum class VideoPipelineMode {
    Auto,
    Child,
    Managed,
    Worker,
};

VideoPipelineMode video_pipeline_mode_from_environment() {
    auto mode = getenv_string("ARCHSTREAMER_VIDEO_PIPELINE");
    std::transform(mode.begin(), mode.end(), mode.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    if (mode.empty() || mode == "worker" || mode == "managed-worker") {
        return VideoPipelineMode::Worker;
    }
    if (mode == "managed" || mode == "in-process" || mode == "inprocess") {
        return VideoPipelineMode::Managed;
    }
    if (mode == "child" || mode == "gst-launch") {
        return VideoPipelineMode::Child;
    }
    if (mode == "auto") {
        return VideoPipelineMode::Auto;
    }
    return VideoPipelineMode::Worker;
}

struct SharedVideoSource {
    enum class Kind { X11, PipeWire };
    Kind kind = Kind::X11;
    std::string value;
    std::string target_object;
};

struct PipeWireSourceSelector {
    std::string property;
    std::string value;
};

std::vector<PipeWireSourceSelector> pipewire_source_selectors(const SharedVideoSource& source) {
    std::vector<PipeWireSourceSelector> selectors;
    auto append = [&](std::string property, const std::string& value) {
        if (!value.empty()) {
            selectors.push_back(PipeWireSourceSelector{std::move(property), value});
        }
    };
    append("path", source.value);
    append("target-object", source.target_object);
    return selectors;
}

struct SharedVideoBranch {
    VideoEncodeSettings settings;
    std::vector<std::pair<std::string, std::uint16_t>> clients;
};

bool same_sink_list(
    std::vector<std::pair<std::string, std::uint16_t>> left,
    std::vector<std::pair<std::string, std::uint16_t>> right) {
    std::sort(left.begin(), left.end());
    std::sort(right.begin(), right.end());
    return left == right;
}

std::string video_settings_summary(const VideoEncodeSettings& settings) {
    const int bitrate = settings.bitrate_kbps == 0 ? 1500 : settings.bitrate_kbps;
    const int framerate = settings.framerate == 0 ? 60 : static_cast<int>(settings.framerate);
    const int queue_buffers =
        settings.queue_buffers == 0 ? 1 : static_cast<int>(settings.queue_buffers);
    std::ostringstream out;
    out << bitrate << "kbps/" << framerate << "fps";
    if (settings.width > 0 && settings.height > 0) {
        out << "/" << settings.width << "x" << settings.height;
    }
    out << " queue=" << queue_buffers;
    return out.str();
}

void apply_nvenc_environment_to_process(
    ChildProcess& process,
    std::vector<std::string> args,
    int nvenc_cuda_device_id,
    const std::optional<std::string>& stderr_path = std::nullopt) {
    process.start(
        std::move(args),
        nvenc_cuda_device_id >= 0
            ? std::vector<std::pair<std::string, std::string>>{
                  {"CUDA_DEVICE_ORDER", "PCI_BUS_ID"},
                  {"CUDA_VISIBLE_DEVICES", std::to_string(nvenc_cuda_device_id)},
              }
            : std::vector<std::pair<std::string, std::string>>{},
        nvenc_cuda_device_id >= 0
            ? std::vector<std::string>{
                  "__NV_PRIME_RENDER_OFFLOAD",
                  "__NV_PRIME_RENDER_OFFLOAD_PROVIDER",
                  "__GLX_VENDOR_LIBRARY_NAME",
                  "DRI_PRIME",
              }
            : std::vector<std::string>{},
        stderr_path);
}

struct H264BranchOptions {
    bool use_nvenc = false;
    bool managed_nvenc_device_select = false;
    int nvenc_cuda_device_id = -1;
};

void append_h264_branch_args(
    std::vector<std::string>& args,
    const VideoEncodeSettings& settings,
    const std::vector<std::pair<std::string, std::uint16_t>>& clients,
    const H264BranchOptions& options) {
    const int bitrate = settings.bitrate_kbps == 0 ? 1500 : settings.bitrate_kbps;
    const int framerate = settings.framerate == 0 ? 60 : static_cast<int>(settings.framerate);
    const int configured_key_int =
        settings.key_int_max == 0 ? framerate : static_cast<int>(settings.key_int_max);
    const int sixth_sec = std::max(5, framerate / 6);
    const int key_int_max = std::min(configured_key_int, sixth_sec);
    const int queue_buffers =
        settings.queue_buffers == 0 ? 1 : static_cast<int>(settings.queue_buffers);

    args.insert(args.end(), {
        "queue",
        "max-size-buffers=" + std::to_string(queue_buffers),
        "max-size-time=0",
        "max-size-bytes=0",
        "leaky=upstream",
        "!",
    });
    if (settings.width > 0 && settings.height > 0) {
        args.insert(args.end(), {
            "videoscale",
            "method=0",
            "!",
            "video/x-raw,width=" + std::to_string(settings.width) +
                ",height=" + std::to_string(settings.height),
            "!",
        });
    }
    args.insert(args.end(), {
        "videorate",
        "drop-only=true",
        "!",
        "video/x-raw,framerate=" + std::to_string(framerate) + "/1",
        "!",
    });
    if (options.use_nvenc) {
        args.insert(args.end(), {
            "nvh264enc",
            "zerolatency=true",
            std::string("preset=") +
                (settings.nvenc_high_quality ? "low-latency-hq" : "low-latency-hp"),
            "strict-gop=true",
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
            gst_argv_property("option-string", "scenecut=40"),
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
        "config-interval=-1",
        "aggregate-mode=zero-latency",
        "pt=96",
        "!",
        "multiudpsink",
        "clients=" + multiudp_clients_arg(
            multiudp_clients_with_pace_tee(
                clients,
                clients.empty() ? 0 : clients.front().second)),
        "sync=false",
        "async=false",
    });
}

void append_h264_branch_desc(
    std::ostringstream& desc,
    const VideoEncodeSettings& settings,
    const std::vector<std::pair<std::string, std::uint16_t>>& clients,
    const H264BranchOptions& options) {
    const int bitrate = settings.bitrate_kbps == 0 ? 1500 : settings.bitrate_kbps;
    const int framerate = settings.framerate == 0 ? 60 : static_cast<int>(settings.framerate);
    const int configured_key_int =
        settings.key_int_max == 0 ? framerate : static_cast<int>(settings.key_int_max);
    const int sixth_sec = std::max(5, framerate / 6);
    const int key_int_max = std::min(configured_key_int, sixth_sec);
    const int queue_buffers =
        settings.queue_buffers == 0 ? 1 : static_cast<int>(settings.queue_buffers);

    desc
        << "queue max-size-buffers=" << queue_buffers
        << " max-size-time=0 max-size-bytes=0 leaky=upstream ! ";
    if (settings.width > 0 && settings.height > 0) {
        desc
            << "videoscale method=0 ! video/x-raw,width=" << settings.width
            << ",height=" << settings.height << " ! ";
    }
    desc
        << "videorate drop-only=true ! video/x-raw,framerate="
        << framerate << "/1 ! ";
    if (options.use_nvenc) {
        if (options.managed_nvenc_device_select) {
            desc
                << "nvautogpuh264enc cuda-device-id=" << options.nvenc_cuda_device_id
                << " zero-reorder-delay=true preset="
                << (settings.nvenc_high_quality ? "low-latency-hq" : "low-latency-hp")
                << " strict-gop=true bitrate=" << bitrate
                << " gop-size=" << key_int_max
                << " repeat-sequence-header=true"
                << " ! video/x-h264,profile=baseline,stream-format=byte-stream";
        } else {
            desc
                << "nvh264enc zerolatency=true preset="
                << (settings.nvenc_high_quality ? "low-latency-hq" : "low-latency-hp")
                << " strict-gop=true bitrate=" << bitrate
                << " gop-size=" << key_int_max
                << " ! video/x-h264,profile=baseline,stream-format=byte-stream";
        }
    } else {
        desc
            << "x264enc tune=zerolatency speed-preset=ultrafast"
            << " bitrate=" << bitrate
            << " key-int-max=" << key_int_max
            << " byte-stream=true bframes=0 threads=1"
            << " " << gst_parse_property("option-string", "scenecut=40")
            << " ! video/x-h264,profile=constrained-baseline,stream-format=byte-stream";
    }
    if (gst_element_available("h264parse")) {
        desc << " ! h264parse config-interval=-1";
    }
    desc
        << " ! rtph264pay mtu=1200 config-interval=-1"
        << " aggregate-mode=zero-latency pt=96"
        << " ! multiudpsink clients="
        << multiudp_clients_arg(
               multiudp_clients_with_pace_tee(
                   clients,
                   clients.empty() ? 0 : clients.front().second))
        << " sync=false async=false";
}

void log_video_ladder(
    const char* mode,
    const SharedVideoSource& source,
    const std::vector<SharedVideoBranch>& branches,
    bool nvenc,
    int nvenc_cuda_device_id,
    int attempt = 1) {
    std::cout << "Video ladder ("
              << (source.kind == SharedVideoSource::Kind::PipeWire ? "pipewire" : "ximagesrc")
              << (nvenc ? ", nvenc" : ", x264")
              << ", " << mode;
    if (source.kind == SharedVideoSource::Kind::PipeWire) {
        std::cout << " path=" << source.value;
        if (!source.target_object.empty()) {
            std::cout << " target-object=" << source.target_object;
        }
        if (attempt > 1) {
            std::cout << " attempt=" << attempt;
        }
    }
    if (nvenc && nvenc_cuda_device_id >= 0) {
        std::cout << " cuda=" << nvenc_cuda_device_id;
    }
    std::cout << "):";
    for (const auto& branch : branches) {
        const auto& s = branch.settings;
        std::cout << " " << branch.clients.size()
                  << "@" << s.bitrate_kbps << "kbps/"
                  << static_cast<int>(s.framerate) << "fps";
        if (s.width > 0 && s.height > 0) {
            std::cout << "/" << s.width << "x" << s.height;
        }
    }
    std::cout << '\n';
}

const char* h264_encoder_name(const H264BranchOptions& options) {
    if (!options.use_nvenc) {
        return "x264enc";
    }
    return options.managed_nvenc_device_select ? "nvautogpuh264enc" : "nvh264enc";
}

std::string managed_pipeline_summary(
    const SharedVideoSource& source,
    std::string_view selector_label,
    const std::vector<SharedVideoBranch>& branches,
    const H264BranchOptions& options,
    int attempt) {
    std::ostringstream out;
    out << "source="
        << (source.kind == SharedVideoSource::Kind::PipeWire ? "pipewire" : "ximagesrc");
    if (!selector_label.empty()) {
        out << " " << selector_label;
    } else if (!source.value.empty()) {
        out << " " << source.value;
    }
    out << " encoder=" << h264_encoder_name(options);
    if (options.use_nvenc && options.nvenc_cuda_device_id >= 0) {
        out << " cuda=" << options.nvenc_cuda_device_id;
    }
    if (attempt > 1) {
        out << " attempt=" << attempt;
    }
    out << " branches=" << branches.size();
    for (const auto& branch : branches) {
        out << " [" << video_settings_summary(branch.settings)
            << " -> " << multiudp_clients_arg(branch.clients) << "]";
    }
    return out.str();
}

class ChildProcessVideoPipelineRunner final : public SharedVideoPipelineRunner {
public:
    void stop() override {
        process_.stop();
    }

    [[nodiscard]] bool running() const override {
        return process_.running();
    }

    bool start(
        const SharedVideoSource& source,
        const std::vector<SharedVideoBranch>& branches,
        const H264BranchOptions& options,
        const std::optional<std::string>& stderr_path = std::nullopt) {
        stop();
        auto args = std::vector<std::string>{"gst-launch-1.0", "-q"};
        if (source.kind == SharedVideoSource::Kind::PipeWire) {
            args.insert(args.end(), {
                "pipewiresrc",
                gst_argv_property("path", source.value),
                "do-timestamp=true",
                "!",
                "video/x-raw,format=BGRx",
                "!",
                "videoconvert",
            });
        } else {
            args.insert(args.end(), {
                "ximagesrc",
                gst_argv_property("display-name", source.value),
                "use-damage=false",
                "show-pointer=false",
                "do-timestamp=true",
                "!",
                "videoconvert",
            });
        }

        if (branches.size() == 1) {
            args.push_back("!");
            append_h264_branch_args(args, branches[0].settings, branches[0].clients, options);
        } else {
            args.insert(args.end(), {"!", "tee", "name=t"});
            for (const auto& branch : branches) {
                args.push_back("t.");
                args.push_back("!");
                append_h264_branch_args(args, branch.settings, branch.clients, options);
            }
        }

        apply_nvenc_environment_to_process(
            process_,
            std::move(args),
            options.nvenc_cuda_device_id,
            stderr_path);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        if (!process_.running()) {
            throw std::runtime_error(
                source.kind == SharedVideoSource::Kind::PipeWire
                    ? "video capture pipeline exited immediately (need pipewiresrc, x264enc/nvh264enc, multiudpsink)"
                    : "video capture pipeline exited immediately (need Xvfb/Xephyr, ximagesrc, x264enc, multiudpsink)");
        }
        log_video_ladder(
            "gst-launch",
            source,
            branches,
            options.use_nvenc,
            options.nvenc_cuda_device_id);
        return true;
    }

private:
    ChildProcess process_;
};

class ManagedGStreamerVideoPipelineRunner final : public SharedVideoPipelineRunner {
public:
    ManagedGStreamerVideoPipelineRunner() {
#if defined(ARCHSTREAMER_HAS_GST_LIBS)
        engine_.set_event_handler([](const GstVideoEvent& event) {
            switch (event.kind) {
            case GstVideoEventKind::Error:
                std::cerr << "Managed GStreamer video runtime error: " << event.message << '\n';
                break;
            case GstVideoEventKind::Warning:
                std::cerr << "Managed GStreamer video warning: " << event.message << '\n';
                break;
            case GstVideoEventKind::Eos:
                std::cerr << "Managed GStreamer video runtime EOS\n";
                break;
            default:
                break;
            }
        });
#endif
    }

    ~ManagedGStreamerVideoPipelineRunner() override {
        stop();
    }

    void stop() override {
#if defined(ARCHSTREAMER_HAS_GST_LIBS)
        engine_.execute(gst_video_stop_command());
#endif
    }

    [[nodiscard]] bool running() const override {
#if defined(ARCHSTREAMER_HAS_GST_LIBS)
        return engine_.running();
#else
        return false;
#endif
    }

    bool start(
        const SharedVideoSource& source,
        const std::vector<SharedVideoBranch>& branches,
        const H264BranchOptions& options) {
        pin_nvenc_cuda_device_order();
        stop();
#if defined(ARCHSTREAMER_HAS_GST_LIBS)
        GstVideoGraph graph;
        graph.source.kind = source.kind == SharedVideoSource::Kind::PipeWire
            ? GstVideoSourceKind::PipeWire
            : GstVideoSourceKind::X11;
        graph.source.value = source.value;
        graph.source.target_object = source.target_object;
        graph.nvenc_cuda_device_id = options.nvenc_cuda_device_id;
        graph.branches.reserve(branches.size());
        for (const auto& branch : branches) {
            GstVideoBranch gst_branch;
            gst_branch.encode = GstVideoEncode{
                branch.settings.bitrate_kbps,
                branch.settings.framerate,
                branch.settings.key_int_max,
                branch.settings.width,
                branch.settings.height,
                branch.settings.queue_buffers,
                branch.settings.nvenc_high_quality,
            };
            for (const auto& [host, port] : branch.clients) {
                gst_branch.sinks.push_back(GstVideoSink{host, port});
            }
            graph.branches.push_back(std::move(gst_branch));
        }
        const auto summary = managed_pipeline_summary(
            source, {}, branches, options, 1);
        std::cout << "Managed GStreamer video pipeline: " << summary << '\n';
        if (getenv_bool("ARCHSTREAMER_GST_VERBOSE")) {
            std::cout
                << "Managed GStreamer pipeline graph: "
                << gst_video_graph_summary(graph) << '\n';
        }
        const auto reply = engine_.execute(
            gst_video_graph_command(GstVideoCommandVerb::Start, std::move(graph), summary));
        if (reply.kind == GstVideoEventKind::Ready) {
            log_video_ladder(
                "in-process",
                source,
                branches,
                options.use_nvenc,
                options.nvenc_cuda_device_id);
            return true;
        }
        last_error_ = reply.message.empty()
            ? "GStreamer engine failed to start"
            : reply.message;
        std::cerr << "Managed GStreamer video failed: " << last_error_ << '\n';
        return false;
#else
        (void)source;
        (void)branches;
        (void)options;
        last_error_ = "GStreamer libraries are not available in this build";
        std::cerr << "Managed GStreamer video failed: " << last_error_ << '\n';
        return false;
#endif
    }

    [[nodiscard]] const std::string& last_error() const {
        return last_error_;
    }

private:
#if defined(ARCHSTREAMER_HAS_GST_LIBS)
    GstVideoEngine engine_;
#endif
    std::string last_error_;
};

std::string gst_worker_executable() {
    if (const char* env = std::getenv("ARCHSTREAMER_GST_WORKER");
        env != nullptr && env[0] != '\0' && access(env, X_OK) == 0) {
        return env;
    }
    char self[4096];
    const ssize_t length = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (length > 0) {
        self[length] = '\0';
        const auto sibling = std::filesystem::path(self).parent_path() / "archstreamer_gst_worker";
        if (access(sibling.c_str(), X_OK) == 0) {
            return sibling.string();
        }
    }
    return "archstreamer_gst_worker";
}

#if defined(ARCHSTREAMER_HAS_GST_LIBS)
GstVideoGraph gst_graph_from_branches(
    const SharedVideoSource& source,
    const std::vector<SharedVideoBranch>& branches,
    const H264BranchOptions& options) {
    GstVideoGraph graph;
    graph.source.kind = source.kind == SharedVideoSource::Kind::PipeWire
        ? GstVideoSourceKind::PipeWire
        : GstVideoSourceKind::X11;
    graph.source.value = source.value;
    graph.source.target_object = source.target_object;
    graph.nvenc_cuda_device_id = options.nvenc_cuda_device_id;
    graph.branches.reserve(branches.size());
    for (const auto& branch : branches) {
        GstVideoBranch gst_branch;
        gst_branch.encode = GstVideoEncode{
            branch.settings.bitrate_kbps,
            branch.settings.framerate,
            branch.settings.key_int_max,
            branch.settings.width,
            branch.settings.height,
            branch.settings.queue_buffers,
            branch.settings.nvenc_high_quality,
        };
        for (const auto& [host, port] : branch.clients) {
            gst_branch.sinks.push_back(GstVideoSink{host, port});
        }
        graph.branches.push_back(std::move(gst_branch));
    }
    return graph;
}
#endif

class WorkerVideoPipelineRunner final : public SharedVideoPipelineRunner {
public:
    ~WorkerVideoPipelineRunner() override {
        stop();
    }

    void stop() override {
#if defined(ARCHSTREAMER_HAS_GST_LIBS)
        client_.stop();
#endif
    }

    [[nodiscard]] bool running() const override {
#if defined(ARCHSTREAMER_HAS_GST_LIBS)
        return client_.running();
#else
        return false;
#endif
    }

    bool start(
        const SharedVideoSource& source,
        const std::vector<SharedVideoBranch>& branches,
        const H264BranchOptions& options) {
        pin_nvenc_cuda_device_order();
        stop();
#if defined(ARCHSTREAMER_HAS_GST_LIBS)
        const auto executable = gst_worker_executable();
        try {
            client_.set_event_handler([](const GstVideoEvent& event) {
                switch (event.kind) {
                case GstVideoEventKind::Error:
                    std::cerr << "GStreamer worker runtime error: " << event.message << '\n';
                    break;
                case GstVideoEventKind::Warning:
                    std::cerr << "GStreamer worker warning: " << event.message << '\n';
                    break;
                case GstVideoEventKind::Eos:
                    std::cerr << "GStreamer worker runtime EOS\n";
                    break;
                default:
                    break;
                }
            });
            client_.start(executable);
        } catch (const std::exception& error) {
            last_error_ = error.what();
            std::cerr << "GStreamer worker failed: " << last_error_ << '\n';
            return false;
        }

        auto graph = gst_graph_from_branches(source, branches, options);
        const auto summary = managed_pipeline_summary(source, {}, branches, options, 1);
        std::cout << "GStreamer worker video pipeline: " << summary << '\n';
        const auto reply = client_.execute(
            gst_video_graph_command(GstVideoCommandVerb::Start, std::move(graph), summary));
        if (reply.kind == GstVideoEventKind::Ready) {
            pipeline_id_ = gst_video_event_pipeline_id(reply.message);
            std::cout << "GStreamer worker pid=" << client_.pid();
            if (!pipeline_id_.empty()) {
                std::cout
                    << " pipeline=" << pipeline_id_
                    << " record=" << gst_video_pipeline_record_path(pipeline_id_);
            }
            std::cout << '\n';
            log_video_ladder(
                "gst-worker",
                source,
                branches,
                options.use_nvenc,
                options.nvenc_cuda_device_id);
            return true;
        }
        last_error_ = reply.message.empty() ? "GStreamer worker failed to start" : reply.message;
        std::cerr << "GStreamer worker video failed: " << last_error_ << '\n';
        client_.stop();
        return false;
#else
        (void)source;
        (void)branches;
        (void)options;
        last_error_ = "GStreamer libraries are not available in this build";
        std::cerr << "GStreamer worker video failed: " << last_error_ << '\n';
        return false;
#endif
    }

    [[nodiscard]] const std::string& last_error() const {
        return last_error_;
    }

private:
#if defined(ARCHSTREAMER_HAS_GST_LIBS)
    GstVideoWorkerClient client_;
    std::string pipeline_id_;
#endif
    std::string last_error_;
};

enum class VideoPipelineUse {
    Shared,
    Staging,
};

const char* video_pipeline_use_name(VideoPipelineUse use) {
    switch (use) {
    case VideoPipelineUse::Shared:
        return "shared";
    case VideoPipelineUse::Staging:
        return "staging";
    }
    return "unknown";
}

H264BranchOptions probe_h264_branch_options(int nvenc_cuda_device_id) {
#if defined(ARCHSTREAMER_HAS_GST_LIBS)
    const auto probed = gst_video_probe_h264(nvenc_cuda_device_id);
    return H264BranchOptions{
        probed.use_nvenc,
        probed.managed_nvenc_device_select,
        probed.nvenc_cuda_device_id,
    };
#else
    (void)nvenc_cuda_device_id;
    return {};
#endif
}

bool managed_inprocess_can_run(const H264BranchOptions& options) {
    return !options.use_nvenc ||
        options.nvenc_cuda_device_id < 0 ||
        options.managed_nvenc_device_select;
}

std::unique_ptr<SharedVideoPipelineRunner> start_selected_video_pipeline(
    const SharedVideoSource& source,
    const std::vector<SharedVideoBranch>& branches,
    const H264BranchOptions& options,
    VideoPipelineUse use,
    const std::optional<std::string>& stderr_path = std::nullopt) {
    const auto pipeline_mode = video_pipeline_mode_from_environment();
    const bool use_managed_inprocess =
        pipeline_mode == VideoPipelineMode::Managed ||
        (pipeline_mode == VideoPipelineMode::Auto && managed_inprocess_can_run(options));

    const auto* label = video_pipeline_use_name(use);
    if (pipeline_mode == VideoPipelineMode::Worker) {
        std::cout << "Video pipeline runner (" << label << "): gst worker process\n";
        auto worker = std::make_unique<WorkerVideoPipelineRunner>();
        if (worker->start(source, branches, options)) {
            return worker;
        }
        throw std::runtime_error(
            std::string{"gst worker video pipeline failed for "} +
            label + ": " + worker->last_error());
    }
    if (use_managed_inprocess) {
        std::cout
            << "Video pipeline runner (" << label << "): in-process GStreamer"
            << (pipeline_mode == VideoPipelineMode::Managed ? " (forced)" : " (auto)")
            << '\n';
        auto managed = std::make_unique<ManagedGStreamerVideoPipelineRunner>();
        if (managed->start(source, branches, options)) {
            return managed;
        }
        if (pipeline_mode == VideoPipelineMode::Managed) {
            throw std::runtime_error(
                std::string{"forced in-process video pipeline failed for "} +
                label + "; gst-launch fallback disabled: " + managed->last_error());
        }
        std::cerr
            << "In-process video pipeline failed for " << label
            << "; falling back to gst-launch\n";
    } else if (pipeline_mode == VideoPipelineMode::Child) {
        std::cout << "Video pipeline runner (" << label << "): gst-launch child process (forced)\n";
    } else {
        std::cerr
            << "In-process video pipeline skipped for " << label
            << ": explicit NVENC CUDA device requested but this GStreamer nvcodec build has "
            << "no writable per-element device property; using gst-launch to preserve GPU binding\n";
    }

    auto child = std::make_unique<ChildProcessVideoPipelineRunner>();
    child->start(source, branches, options, stderr_path);
    return child;
}

bool pipeline_running(const std::unique_ptr<SharedVideoPipelineRunner>& pipeline) {
    return pipeline != nullptr && pipeline->running();
}

void stop_pipeline(std::unique_ptr<SharedVideoPipelineRunner>& pipeline) {
    if (pipeline != nullptr) {
        pipeline->stop();
        pipeline.reset();
    }
}

} // namespace

GStreamerVideoFanout::~GStreamerVideoFanout() {
    stop();
}

GStreamerVideoFanout::Destination* GStreamerVideoFanout::find_destination(ClientId client_id) {
    for (auto& destination : destinations_) {
        if (destination.client_id == client_id) {
            return &destination;
        }
    }
    return nullptr;
}

const GStreamerVideoFanout::Destination* GStreamerVideoFanout::find_destination(
    ClientId client_id) const {
    for (const auto& destination : destinations_) {
        if (destination.client_id == client_id) {
            return &destination;
        }
    }
    return nullptr;
}

std::string GStreamerVideoFanout::staging_encode_log_path() {
    const auto directory = std::filesystem::path{archstreamer_cache_directory()};
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    return (directory / "gst-video-staging-encode.log").string();
}

std::vector<MediaClientStream> GStreamerVideoFanout::start(
    const std::string& display,
    const std::vector<MediaStreamRequest>& destinations,
    const VideoEncodeSettings& initial_settings) {
    if (!destinations_.empty() || shared_pipeline_running()) {
        throw std::runtime_error("video fanout is already running");
    }

    source_kind_ = SourceKind::X11;
    display_ = display;
    pipewire_target_ = {};
    auto streams = std::vector<MediaClientStream>{};
    streams.reserve(destinations.size());
    for (const auto& destination : destinations) {
        Destination slot{};
        slot.client_id = destination.client_id;
        slot.host = destination.destination_host;
        slot.base_port = destination.port;
        slot.port = destination.port;
        slot.settings = initial_settings.bitrate_kbps == 0
            ? video_encode_settings(MediaStreamSize::P720, MediaQualityTier::Medium)
            : initial_settings;
        destinations_.push_back(std::move(slot));
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
    const GamescopePipeWireTarget& pipewire_target,
    const std::vector<MediaStreamRequest>& destinations,
    const VideoEncodeSettings& initial_settings) {
    if (!destinations_.empty() || shared_pipeline_running()) {
        throw std::runtime_error("video fanout is already running");
    }

    source_kind_ = SourceKind::PipeWire;
    display_.clear();
    pipewire_target_ = pipewire_target;
    auto streams = std::vector<MediaClientStream>{};
    streams.reserve(destinations.size());
    for (const auto& destination : destinations) {
        Destination slot{};
        slot.client_id = destination.client_id;
        slot.host = destination.destination_host;
        slot.base_port = destination.port;
        slot.port = destination.port;
        slot.settings = initial_settings.bitrate_kbps == 0
            ? video_encode_settings(MediaStreamSize::P720, MediaQualityTier::Medium)
            : initial_settings;
        destinations_.push_back(std::move(slot));
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
    slot.base_port = destination.port;
    slot.port = destination.port;
    slot.settings = settings.bitrate_kbps == 0
        ? video_encode_settings(MediaStreamSize::P720, MediaQualityTier::Medium)
        : settings;
    destinations_.push_back(std::move(slot));
    restart_pipeline();

    return MediaClientStream{
        destination.client_id,
        destination.destination_host,
        MediaEndpoint{rtp_h264_uri(destination.destination_host, destination.port), ""},
    };
}

bool GStreamerVideoFanout::reconfigure_shared(const VideoEncodeSettings& settings) {
    if (destinations_.empty()) {
        return false;
    }
    if (source_kind_ == SourceKind::X11 && display_.empty()) {
        return false;
    }
    if (source_kind_ == SourceKind::PipeWire && pipewire_target_.path.empty()) {
        return false;
    }

    bool already_configured = shared_pipeline_running();
    for (const auto& destination : destinations_) {
        if (destination.staging_active ||
            pipeline_running(destination.dedicated) ||
            destination.port != destination.base_port ||
            destination.settings != settings) {
            already_configured = false;
            break;
        }
    }
    if (already_configured) {
        return true;
    }

    for (auto& destination : destinations_) {
        if (destination.staging_active) {
            stop_pipeline(destination.staging);
            if (destination.staging_port != 0) {
                terminate_gst_multiudpsink_on_port(destination.staging_port);
                rtp_frame_pace_debug::stop_tee(destination.staging_port);
            }
            destination.staging_active = false;
            destination.staging_port = 0;
        }
        stop_pipeline(destination.dedicated);
        if (destination.port != destination.base_port) {
            rtp_frame_pace_debug::stop_tee(destination.port);
            destination.port = destination.base_port;
        }
        destination.settings = settings;
    }

    stop_sample_pipelines();
    restart_pipeline();
    const bool ok = shared_pipeline_running();
    if (ok) {
        std::cout
            << "Shared video reconfigured -> "
            << media_stream_size_name(media_stream_size_for_settings(settings))
            << "/" << media_quality_tier_name(media_quality_tier_for_settings(settings))
            << " (" << settings.bitrate_kbps << " kbps, "
            << static_cast<int>(settings.framerate) << " fps";
        if (settings.width > 0 && settings.height > 0) {
            std::cout << ", " << settings.width << "x" << settings.height;
        }
        std::cout
            << ", queue=" << static_cast<int>(settings.queue_buffers)
            << ", nvenc=" << (settings.nvenc_high_quality ? "hq" : "hp")
            << ", clients=" << destinations_.size() << ")\n";
    } else {
        std::cerr << "Shared video reconfigure failed to start pipeline\n";
    }
    return ok;
}

bool GStreamerVideoFanout::apply_branch_layout(
    const VideoEncodeSettings& trunk,
    const std::vector<std::pair<ClientId, VideoEncodeSettings>>& per_client) {
    if (destinations_.empty()) {
        return false;
    }
    if (source_kind_ == SourceKind::X11 && display_.empty()) {
        return false;
    }
    if (source_kind_ == SourceKind::PipeWire && pipewire_target_.path.empty()) {
        return false;
    }

    bool already_configured = shared_pipeline_running();
    for (const auto& destination : destinations_) {
        auto expected = trunk;
        for (const auto& [client_id, settings] : per_client) {
            if (client_id == destination.client_id) {
                expected = settings;
                break;
            }
        }
        if (destination.staging_active ||
            pipeline_running(destination.dedicated) ||
            destination.port != destination.base_port ||
            destination.settings != expected) {
            already_configured = false;
            break;
        }
    }
    if (already_configured) {
        last_layout_rebuilt_ = false;
        return true;
    }

    for (auto& destination : destinations_) {
        if (destination.staging_active) {
            stop_pipeline(destination.staging);
            if (destination.staging_port != 0) {
                terminate_gst_multiudpsink_on_port(destination.staging_port);
                rtp_frame_pace_debug::stop_tee(destination.staging_port);
            }
            destination.staging_active = false;
            destination.staging_port = 0;
        }
        stop_pipeline(destination.dedicated);
        if (destination.port != destination.base_port) {
            rtp_frame_pace_debug::stop_tee(destination.port);
            destination.port = destination.base_port;
        }
        destination.settings = trunk;
    }

    for (const auto& [client_id, settings] : per_client) {
        if (Destination* slot = find_destination(client_id); slot != nullptr) {
            slot->settings = settings;
        }
    }

    apply_encode_tree(trunk);
    const bool ok = shared_pipeline_running();
    last_layout_rebuilt_ = ok;
    if (ok) {
        std::cout << "Video branch layout applied (trunk "
                  << trunk.bitrate_kbps << "kbps/"
                  << static_cast<int>(trunk.framerate) << "fps";
        if (trunk.width > 0 && trunk.height > 0) {
            std::cout << "/" << trunk.width << "x" << trunk.height;
        }
        std::cout << ", clients=" << destinations_.size() << ")\n";
    } else {
        std::cerr << "Video branch layout failed to start pipeline\n";
    }
    return ok;
}

std::optional<std::string> GStreamerVideoFanout::begin_tier_cutover(
    ClientId client_id,
    const VideoEncodeSettings& settings) {
    Destination* slot = find_destination(client_id);
    if (slot == nullptr) {
        return std::nullopt;
    }
    if (source_kind_ == SourceKind::X11 && display_.empty()) {
        return std::nullopt;
    }
    if (source_kind_ == SourceKind::PipeWire && pipewire_target_.path.empty()) {
        return std::nullopt;
    }

    if (slot->staging_active) {
        stop_pipeline(slot->staging);
        if (slot->staging_port != 0) {
            terminate_gst_multiudpsink_on_port(slot->staging_port);
            rtp_frame_pace_debug::stop_tee(slot->staging_port);
        }
        slot->staging_active = false;
        slot->staging_port = 0;
    }

    auto staging_port = static_cast<std::uint16_t>(slot->base_port + StagingPortOffset);
    if (staging_port == slot->port) {
        staging_port = static_cast<std::uint16_t>(slot->base_port + AlternateStagingPortOffset);
    }
    if (staging_port == slot->port || staging_port == slot->base_port) {
        std::cerr
            << "Video staging could not find a free alternate port for client "
            << static_cast<int>(client_id) << '\n';
        return std::nullopt;
    }
    terminate_gst_multiudpsink_on_port(staging_port);
    rtp_frame_pace_debug::stop_tee(staging_port);
    if (!gst_element_available("multiudpsink")) {
        throw std::runtime_error("multiudpsink is required for video staging (gst-plugins-good)");
    }
    if (source_kind_ == SourceKind::PipeWire && !gst_element_available("pipewiresrc")) {
        throw std::runtime_error("pipewiresrc is required for gamescope video staging");
    }
    const SharedVideoSource source{
        source_kind_ == SourceKind::PipeWire
            ? SharedVideoSource::Kind::PipeWire
            : SharedVideoSource::Kind::X11,
        source_kind_ == SourceKind::PipeWire ? pipewire_target_.path : display_,
        source_kind_ == SourceKind::PipeWire ? pipewire_target_.target_object : std::string{},
    };
    const auto options = probe_h264_branch_options(nvenc_cuda_device_id_);
    const auto branch = SharedVideoBranch{settings, {{slot->host, staging_port}}};
    try {
        slot->staging = start_selected_video_pipeline(
            source,
            {branch},
            options,
            VideoPipelineUse::Staging,
            staging_encode_log_path());
    } catch (const std::exception& ex) {
        std::cerr
            << "Video staging failed for client " << static_cast<int>(client_id)
            << ": " << ex.what() << '\n';
        slot->staging.reset();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    if (!pipeline_running(slot->staging)) {
        slot->staging.reset();
        slot->staging_port = 0;
        return std::nullopt;
    }

    slot->staging_active = true;
    slot->staging_port = staging_port;
    slot->staging_settings = settings;
    slot->staging_started = std::chrono::steady_clock::now();
    std::cerr
        << "Video staging started for client " << static_cast<int>(client_id)
        << " -> " << slot->host << ":" << staging_port << '\n';
    return rtp_h264_uri(slot->host, staging_port);
}

bool GStreamerVideoFanout::complete_tier_cutover(ClientId client_id, std::string_view video_uri) {
    Destination* slot = find_destination(client_id);
    if (slot == nullptr || !slot->staging_active || slot->staging_port == 0) {
        return false;
    }
    const auto expected = rtp_h264_uri(slot->host, slot->staging_port);
    if (video_uri != expected) {
        std::cerr
            << "Video staging URI mismatch for client " << static_cast<int>(client_id)
            << ": got \"" << video_uri << "\", expected \"" << expected << "\"\n";
        return false;
    }

    stop_pipeline(slot->dedicated);
    if (slot->port != slot->base_port) {
        rtp_frame_pace_debug::stop_tee(slot->port);
    }
    slot->dedicated = std::move(slot->staging);
    slot->port = slot->staging_port;
    slot->settings = slot->staging_settings;
    slot->staging_active = false;
    slot->staging_port = 0;
    slot->staging_started = {};
    std::cerr
        << "Video staging promoted for client " << static_cast<int>(client_id)
        << " on port " << slot->port << '\n';

    bool any_shared = false;
    for (const auto& destination : destinations_) {
        if (!pipeline_running(destination.dedicated)) {
            any_shared = true;
            break;
        }
    }
    if (!any_shared) {
        stop_shared_pipeline();
    } else {
        restart_pipeline();
    }
    return true;
}

void GStreamerVideoFanout::abort_tier_cutover(ClientId client_id) {
    Destination* slot = find_destination(client_id);
    if (slot == nullptr || !slot->staging_active) {
        return;
    }
    stop_pipeline(slot->staging);
    if (slot->staging_port != 0) {
        terminate_gst_multiudpsink_on_port(slot->staging_port);
        rtp_frame_pace_debug::stop_tee(slot->staging_port);
    }
    slot->staging_active = false;
    slot->staging_port = 0;
    std::cerr << "Video cutover aborted for client " << static_cast<int>(client_id) << '\n';
}

bool GStreamerVideoFanout::cutover_in_flight(ClientId client_id) const {
    const Destination* slot = find_destination(client_id);
    return slot != nullptr && slot->staging_active;
}

std::optional<std::string> GStreamerVideoFanout::current_video_uri(ClientId client_id) const {
    const Destination* slot = find_destination(client_id);
    if (slot == nullptr || slot->port == 0 || slot->host.empty()) {
        return std::nullopt;
    }
    return rtp_h264_uri(slot->host, slot->port);
}

void GStreamerVideoFanout::stop() {
    for (auto& destination : destinations_) {
        if (destination.staging_active) {
            stop_pipeline(destination.staging);
            destination.staging_active = false;
            if (destination.staging_port != 0) {
                rtp_frame_pace_debug::stop_tee(destination.staging_port);
            }
        }
        stop_pipeline(destination.dedicated);
        rtp_frame_pace_debug::stop_tee(destination.port);
        rtp_frame_pace_debug::stop_tee(destination.base_port);
    }
    stop_sample_pipelines();
    stop_shared_pipeline();
    destinations_.clear();
    display_.clear();
    trunk_clients_.clear();
    rtp_frame_pace_debug::stop_all();
}

void GStreamerVideoFanout::stop_client(ClientId client_id) {
    Destination* slot = find_destination(client_id);
    if (slot == nullptr) {
        return;
    }
    const bool was_on_shared_tee = !pipeline_running(slot->dedicated);
    if (slot->staging_active) {
        abort_tier_cutover(client_id);
    }
    rtp_frame_pace_debug::stop_tee(slot->port);
    rtp_frame_pace_debug::stop_tee(slot->base_port);
    stop_pipeline(slot->dedicated);
    destinations_.erase(
        std::remove_if(
            destinations_.begin(),
            destinations_.end(),
            [client_id](const Destination& destination) {
                return destination.client_id == client_id;
            }),
        destinations_.end());
    if (was_on_shared_tee) {
        if (destinations_.empty()) {
            stop_sample_pipelines();
            stop_shared_pipeline();
            trunk_clients_.clear();
            return;
        }
        // Only rebuild shared tee when the removed client was on it.
        bool any_shared = false;
        for (const auto& destination : destinations_) {
            if (!pipeline_running(destination.dedicated)) {
                any_shared = true;
                break;
            }
        }
        if (any_shared) {
            restart_pipeline();
        } else {
            stop_shared_pipeline();
        }
    }
}

void GStreamerVideoFanout::stop_sample_pipelines() {
    for (auto& sample : sample_encodes_) {
        stop_pipeline(sample.pipeline);
    }
    sample_encodes_.clear();
}

void GStreamerVideoFanout::stop_shared_pipeline() {
    stop_pipeline(shared_pipeline_);
}

bool GStreamerVideoFanout::consume_last_layout_rebuilt() {
    return std::exchange(last_layout_rebuilt_, true);
}

bool GStreamerVideoFanout::shared_pipeline_running() const {
    return shared_pipeline_ != nullptr && shared_pipeline_->running();
}

void GStreamerVideoFanout::restart_pipeline() {
    std::vector<SharedVideoBranch> branches;
    for (const auto& destination : destinations_) {
        if (pipeline_running(destination.dedicated)) {
            continue;
        }
        const auto client = std::make_pair(destination.host, destination.port);
        bool merged = false;
        for (auto& branch : branches) {
            if (branch.settings == destination.settings) {
                branch.clients.push_back(client);
                merged = true;
                break;
            }
        }
        if (!merged) {
            branches.push_back(SharedVideoBranch{destination.settings, {client}});
        }
    }
    if (branches.empty()) {
        stop_sample_pipelines();
        stop_shared_pipeline();
        trunk_clients_.clear();
        return;
    }
    VideoEncodeSettings trunk = branches.front().settings;
    for (const auto& branch : branches) {
        trunk = dominate_video_encode_settings(trunk, branch.settings);
    }
    apply_encode_tree(trunk);
}

void GStreamerVideoFanout::apply_encode_tree(const VideoEncodeSettings& trunk) {
    std::vector<SharedVideoBranch> branches;
    for (const auto& destination : destinations_) {
        if (pipeline_running(destination.dedicated)) {
            continue;
        }
        const auto client = std::make_pair(destination.host, destination.port);
        bool merged = false;
        for (auto& branch : branches) {
            if (branch.settings == destination.settings) {
                branch.clients.push_back(client);
                merged = true;
                break;
            }
        }
        if (!merged) {
            branches.push_back(SharedVideoBranch{destination.settings, {client}});
        }
    }
    if (branches.empty()) {
        stop_sample_pipelines();
        stop_shared_pipeline();
        trunk_clients_.clear();
        return;
    }

    SharedVideoBranch trunk_branch{trunk, {}};
    std::vector<SharedVideoBranch> samples;
    for (const auto& branch : branches) {
        if (branch.settings == trunk) {
            for (const auto& client : branch.clients) {
                trunk_branch.clients.push_back(client);
            }
        } else {
            samples.push_back(branch);
        }
    }
    if (trunk_branch.clients.empty()) {
        trunk_branch = branches.front();
        samples.clear();
        for (std::size_t i = 1; i < branches.size(); ++i) {
            samples.push_back(branches[i]);
        }
    }

    const auto restart_started = std::chrono::steady_clock::now();
    auto log_restart_step = [&](std::string_view step) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - restart_started);
        std::cout << "Video pipeline restart +" << elapsed.count() << "ms: " << step << '\n';
    };

    if (source_kind_ == SourceKind::X11 && display_.empty()) {
        return;
    }
    if (source_kind_ == SourceKind::PipeWire && pipewire_target_.path.empty()) {
        return;
    }
    if (!gst_element_available("multiudpsink")) {
        throw std::runtime_error("multiudpsink is required for video ladder fanout (gst-plugins-good)");
    }
    log_restart_step("multiudpsink available");
    if (source_kind_ == SourceKind::PipeWire && !gst_element_available("pipewiresrc")) {
        throw std::runtime_error("pipewiresrc is required for gamescope video capture (gst-plugin-pipewire)");
    }
    if (source_kind_ == SourceKind::PipeWire) {
        log_restart_step("pipewiresrc available");
        configure_managed_pipewire_environment();
        log_restart_step("pipewire environment configured");
    }

    const auto options = probe_h264_branch_options(nvenc_cuda_device_id_);
    log_restart_step("nvenc probe complete");

    const SharedVideoSource source{
        source_kind_ == SourceKind::PipeWire
            ? SharedVideoSource::Kind::PipeWire
            : SharedVideoSource::Kind::X11,
        source_kind_ == SourceKind::PipeWire ? pipewire_target_.path : display_,
        source_kind_ == SourceKind::PipeWire ? pipewire_target_.target_object : std::string{},
    };

    std::vector<SharedVideoBranch> graph_branches;
    graph_branches.push_back(trunk_branch);
    for (const auto& sample : samples) {
        graph_branches.push_back(sample);
    }

    bool samples_unchanged = sample_encodes_.size() == samples.size();
    if (samples_unchanged) {
        for (const auto& wanted : samples) {
            bool found = false;
            for (const auto& existing : sample_encodes_) {
                if (existing.settings == wanted.settings &&
                    same_sink_list(existing.clients, wanted.clients)) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                samples_unchanged = false;
                break;
            }
        }
    }

    const bool graph_unchanged =
        shared_pipeline_running() &&
        trunk_settings_ == trunk_branch.settings &&
        same_sink_list(trunk_clients_, trunk_branch.clients) &&
        samples_unchanged;
    if (!graph_unchanged) {
        for (const auto& branch : graph_branches) {
            for (const auto& [host, port] : branch.clients) {
                (void)host;
                terminate_gst_multiudpsink_on_port(port);
            }
        }
        stop_sample_pipelines();
        stop_shared_pipeline();
        shared_pipeline_ = start_selected_video_pipeline(
            source,
            graph_branches,
            options,
            VideoPipelineUse::Shared);
        trunk_settings_ = trunk_branch.settings;
        trunk_clients_ = trunk_branch.clients;
        sample_encodes_.clear();
        for (const auto& sample : samples) {
            SampleEncode slot;
            slot.settings = sample.settings;
            slot.clients = sample.clients;
            sample_encodes_.push_back(std::move(slot));
        }
        log_restart_step(
            samples.empty() ? "trunk encode started" : "trunk+sample tee started");
    } else {
        log_restart_step("trunk encode unchanged");
    }

    std::cout
        << "Video encode tree: trunk "
        << trunk_branch.settings.bitrate_kbps << "kbps/"
        << static_cast<int>(trunk_branch.settings.framerate) << "fps";
    if (trunk_branch.settings.width > 0 && trunk_branch.settings.height > 0) {
        std::cout
            << "/" << trunk_branch.settings.width << "x" << trunk_branch.settings.height;
    }
    std::cout
        << " clients=" << trunk_branch.clients.size()
        << " samples=" << sample_encodes_.size()
        << " capture=single-tee\n";
}

GStreamerMediaServer::GStreamerMediaServer(GStreamerMediaCaptureConfig capture)
    : capture_(std::move(capture)) {
}

void GStreamerMediaServer::start(
    const MediaPlanConfig& plan,
    const std::vector<MediaDestination>& destinations,
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
                    std::cout << "gamescope (PipeWire"
                              << (gamescope_headless_backend() ? ", headless" : ", nested window")
                              << ")";
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
                case VirtualDisplayBackend::Desktop:
                    std::cout << "desktop (host capture)";
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
            video_fanout_->set_nvenc_cuda_device_id(capture_.nvenc_cuda_device_id);
            const auto video_streams = video_fanout_->start(
                capture_.virtual_display,
                video_requests_from_media_destinations(plan, destinations),
                plan_.initial_video_settings);
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
    return defer_pipewire_video_ && !video();
}

void GStreamerMediaServer::start_pipewire_video(
    const GamescopePipeWireTarget& pipewire_target,
    std::vector<MediaClientStream>& streams) {
    if (!capture_.video || pipewire_target.path.empty()) {
        return;
    }
    if (video()) {
        video_fanout_->stop();
        video_fanout_.reset();
    }
    video_fanout_.emplace();
    video_fanout_->set_nvenc_cuda_device_id(capture_.nvenc_cuda_device_id);
    const auto video_streams = video_fanout_->start_pipewire(
        pipewire_target,
        video_requests_from_media_destinations(plan_, destinations_),
        plan_.initial_video_settings);
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
    const auto destination = MediaDestination{client_id, destination_host};
    if (wants_video && capture_.video && video()) {
        const auto stream = video_fanout_->add(
            capture_.virtual_display,
            video_request_for_destination(plan_, destination, media_index),
            plan_.initial_video_settings);
        endpoint.video_uri = stream.endpoint.video_uri;
    }
    if (wants_audio && capture_.audio && audio()) {
        const auto stream = audio_fanout_->add(
            capture_.audio_backend,
            capture_.audio_source,
            audio_request_for_destination(plan_, destination, media_index));
        endpoint.audio_uri = stream.endpoint.audio_uri;
    }
    return endpoint;
}

void GStreamerMediaServer::remove_client(ClientId client_id) {
    if (video()) {
        video_fanout_->stop_client(client_id);
    }
    if (audio()) {
        audio_fanout_->stop_client(client_id);
    }
}

bool GStreamerMediaServer::reconfigure_shared_video(const VideoEncodeSettings& settings) {
    if (!video()) {
        return false;
    }
    plan_.initial_video_settings = settings;
    return video_fanout_->reconfigure_shared(settings);
}

bool GStreamerMediaServer::apply_video_branch_layout(
    const VideoEncodeSettings& trunk,
    const std::vector<std::pair<ClientId, VideoEncodeSettings>>& per_client) {
    if (!video()) {
        return false;
    }
    plan_.initial_video_settings = trunk;
    return video_fanout_->apply_branch_layout(trunk, per_client);
}

bool GStreamerMediaServer::consume_video_layout_rebuilt() {
    if (!video()) {
        return false;
    }
    return video_fanout_->consume_last_layout_rebuilt();
}

bool GStreamerMediaServer::restart_shared_audio() {
    if (!audio()) {
        return false;
    }
    try {
        audio_fanout_->restart();
        return true;
    } catch (const std::exception& error) {
        std::cerr << "Shared audio restart failed: " << error.what() << '\n';
        return false;
    }
}

bool GStreamerMediaServer::complete_video_tier_cutover(
    ClientId client_id,
    std::string_view staging_video_uri) {
    if (!video()) {
        return false;
    }
    return video_fanout_->complete_tier_cutover(client_id, staging_video_uri);
}

void GStreamerMediaServer::abort_video_tier_cutover(ClientId client_id) {
    if (video()) {
        video_fanout_->abort_tier_cutover(client_id);
    }
}

bool GStreamerMediaServer::video_cutover_in_flight(ClientId client_id) const {
    return video() && video_fanout_->cutover_in_flight(client_id);
}

std::optional<std::string> GStreamerMediaServer::current_video_uri(ClientId client_id) const {
    if (!video()) {
        return std::nullopt;
    }
    return video_fanout_->current_video_uri(client_id);
}

std::optional<std::string> GStreamerMediaServer::begin_video_tier_cutover(
    ClientId client_id,
    const VideoEncodeSettings& settings) {
    if (!video()) {
        return std::nullopt;
    }
    return video_fanout_->begin_tier_cutover(client_id, settings);
}

void GStreamerMediaServer::stop() {
    if (audio()) {
        audio_fanout_->stop();
        audio_fanout_.reset();
    }
    if (video()) {
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
