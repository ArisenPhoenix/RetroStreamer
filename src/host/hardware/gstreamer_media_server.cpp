#include "common/addresses.hpp"
#include "common/platform/paths.hpp"
#include "common/platform/process_utils.hpp"
#include "host/hardware/gpu_select.hpp"
#include "host/hardware/gstreamer_media_server.hpp"
#include "host/host_launch_planner.hpp"
#include "host/rtp_frame_pace_debug.hpp"

#include <array>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>

#include <unistd.h>

#if defined(ARCHSTREAMER_HAS_GST_LIBS)
#include <gst/gst.h>
#endif

namespace archstreamer {
namespace {

#if defined(ARCHSTREAMER_HAS_GST_LIBS)
std::once_flag g_gst_once;
bool g_gst_available = false;

void ensure_gst_initialized() {
    std::call_once(g_gst_once, [] {
        GError* error = nullptr;
        g_gst_available = gst_init_check(nullptr, nullptr, &error);
        if (error != nullptr) {
            std::cerr << "GStreamer init failed: " << error->message << '\n';
            g_error_free(error);
        }
    });
}

std::string gst_message_error_string(GstMessage* msg) {
    GError* gst_error = nullptr;
    gchar* debug = nullptr;
    gst_message_parse_error(msg, &gst_error, &debug);
    std::string reason = "bus error";
    if (GST_OBJECT_NAME(msg->src) != nullptr) {
        reason += std::string(" at ") + GST_OBJECT_NAME(msg->src);
    }
    reason += ": ";
    reason += gst_error != nullptr ? gst_error->message : "unknown error";
    if (debug != nullptr) {
        reason += " | debug: ";
        reason += debug;
    }
    if (gst_error != nullptr) {
        g_error_free(gst_error);
    }
    if (debug != nullptr) {
        g_free(debug);
    }
    return reason;
}
#endif

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

std::string gst_quote(std::string_view value) {
    std::string quoted = "'";
    for (const char ch : value) {
        if (ch == '\\' || ch == '\'') {
            quoted.push_back('\\');
        }
        quoted.push_back(ch);
    }
    quoted.push_back('\'');
    return quoted;
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

struct GstInspectCacheEntry {
    bool available = false;
    std::string output;
};

const GstInspectCacheEntry& gst_inspect_element_cached(const std::string& element) {
    static std::mutex cache_mutex;
    static std::unordered_map<std::string, GstInspectCacheEntry> cache;

    std::lock_guard lock(cache_mutex);
    if (const auto found = cache.find(element); found != cache.end()) {
        return found->second;
    }

    auto entry = GstInspectCacheEntry{};
    static const bool inspect_available = command_available("gst-inspect-1.0");
    if (inspect_available) {
        entry.output = read_command_output(
            (std::string("gst-inspect-1.0 ") + element + " 2>/dev/null").c_str());
        entry.available = !entry.output.empty();
    }
    return cache.emplace(element, std::move(entry)).first->second;
}

bool gst_element_available(const char* element) {
    if (element == nullptr || element[0] == '\0') {
        return false;
    }
    return gst_inspect_element_cached(element).available;
}

bool gst_element_has_writable_property(const char* element, const char* property) {
    if (element == nullptr || element[0] == '\0' ||
        property == nullptr || property[0] == '\0') {
        return false;
    }
    const auto& probe = gst_inspect_element_cached(element);
    if (!probe.available) {
        return false;
    }
    return probe.output.find(std::string("\n  ") + property) != std::string::npos;
}

enum class VideoPipelineMode {
    Auto,
    Child,
    Managed,
};

VideoPipelineMode video_pipeline_mode_from_environment() {
    auto mode = getenv_string("ARCHSTREAMER_VIDEO_PIPELINE");
    std::transform(mode.begin(), mode.end(), mode.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    // temporarily default to Managed to test it without confusion in envs, later will use this though
    return VideoPipelineMode::Managed;
    if (mode == "managed" || mode == "in-process" || mode == "inprocess") {
        return VideoPipelineMode::Managed;
    }
    if (mode == "child" || mode == "gst-launch") {
        return VideoPipelineMode::Child;
    }
    if (mode == "auto") {
        return VideoPipelineMode::Auto;
    }
    return VideoPipelineMode::Child;
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

std::string video_settings_summary(const VideoEncodeSettings& settings) {
    const int bitrate = settings.bitrate_kbps == 0 ? 1500 : settings.bitrate_kbps;
    const int framerate = settings.framerate == 0 ? 30 : static_cast<int>(settings.framerate);
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
    const int framerate = settings.framerate == 0 ? 30 : static_cast<int>(settings.framerate);
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
            "option-string=scenecut=40",
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
    const int framerate = settings.framerate == 0 ? 30 : static_cast<int>(settings.framerate);
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
            << " option-string=" << gst_quote("scenecut=40")
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
                "path=" + source.value,
                "do-timestamp=true",
                "!",
                "video/x-raw,format=BGRx",
                "!",
                "videoconvert",
            });
        } else {
            args.insert(args.end(), {
                "ximagesrc",
                "display-name=" + source.value,
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
    ~ManagedGStreamerVideoPipelineRunner() override {
        stop();
    }

    void stop() override {
#if defined(ARCHSTREAMER_HAS_GST_LIBS)
        monitor_running_ = false;
        if (monitor_.joinable()) {
            monitor_.join();
        }
        if (pipeline_ != nullptr) {
            auto* pipeline = static_cast<GstElement*>(pipeline_);
            gst_element_set_state(pipeline, GST_STATE_NULL);
            gst_element_get_state(pipeline, nullptr, nullptr, 2 * GST_SECOND);
            gst_object_unref(pipeline);
            pipeline_ = nullptr;
        }
#endif
    }

    [[nodiscard]] bool running() const override {
#if defined(ARCHSTREAMER_HAS_GST_LIBS)
        if (pipeline_ != nullptr) {
            GstState state = GST_STATE_NULL;
            auto* pipeline = static_cast<GstElement*>(pipeline_);
            gst_element_get_state(pipeline, &state, nullptr, 0);
            return state == GST_STATE_PLAYING || state == GST_STATE_PAUSED;
        }
#endif
        return false;
    }

    bool start(
        const SharedVideoSource& source,
        const std::vector<SharedVideoBranch>& branches,
        const H264BranchOptions& options) {
        pin_nvenc_cuda_device_order();
        stop();
        constexpr int PipeWireStartupAttempts = 8;
        constexpr auto PipeWireStartupRetryDelay = std::chrono::milliseconds(250);
        const int startup_attempts =
            source.kind == SharedVideoSource::Kind::PipeWire ? PipeWireStartupAttempts : 1;
        const auto selectors = source.kind == SharedVideoSource::Kind::PipeWire
            ? pipewire_source_selectors(source)
            : std::vector<PipeWireSourceSelector>{};

        std::string managed_errors;
        for (int attempt = 1; attempt <= startup_attempts; ++attempt) {
            if (attempt > 1) {
                std::this_thread::sleep_for(PipeWireStartupRetryDelay);
            }
            const int selector_count = source.kind == SharedVideoSource::Kind::PipeWire
                ? static_cast<int>(selectors.size())
                : 1;
            for (int selector_index = 0; selector_index < selector_count; ++selector_index) {
                std::ostringstream desc;
                std::string selector_label;
                if (source.kind == SharedVideoSource::Kind::PipeWire) {
                    const auto& selector = selectors[selector_index];
                    selector_label = selector.property + "=" + selector.value;
                    desc
                        << "pipewiresrc " << selector.property << "=" << selector.value
                        << " do-timestamp=true ! video/x-raw,format=BGRx ! videoconvert";
                } else {
                    desc
                        << "ximagesrc display-name=" << gst_quote(source.value)
                        << " use-damage=false show-pointer=false do-timestamp=true ! videoconvert";
                }
                if (branches.size() == 1) {
                    desc << " ! ";
                    append_h264_branch_desc(
                        desc, branches[0].settings, branches[0].clients, options);
                } else {
                    desc << " ! tee name=t";
                    for (const auto& branch : branches) {
                        desc << " t. ! ";
                        append_h264_branch_desc(desc, branch.settings, branch.clients, options);
                    }
                }

                const auto summary = managed_pipeline_summary(
                    source,
                    selector_label,
                    branches,
                    options,
                    attempt);
                std::string managed_error;
                if (start_description(desc.str(), summary, &managed_error)) {
                    log_video_ladder(
                        "in-process",
                        source,
                        branches,
                        options.use_nvenc,
                        options.nvenc_cuda_device_id,
                        attempt);
                    return true;
                }
                if (!managed_errors.empty()) {
                    managed_errors += " | ";
                }
                managed_errors += "attempt " + std::to_string(attempt);
                if (!selector_label.empty()) {
                    managed_errors += " " + selector_label;
                }
                managed_errors += ": " + managed_error;
            }
        }
        last_error_ = managed_errors;
        return false;
    }

    [[nodiscard]] const std::string& last_error() const {
        return last_error_;
    }

private:
    bool start_description(
        std::string_view description,
        std::string_view summary,
        std::string* error_out) {
        auto fail = [&](std::string reason) {
            if (error_out != nullptr) {
                *error_out = reason;
            }
            std::cerr << "Managed GStreamer video failed: " << reason << '\n';
            return false;
        };
#if defined(ARCHSTREAMER_HAS_GST_LIBS)
        ensure_gst_initialized();
        if (g_gst_available) {
            std::cout << "Managed GStreamer video pipeline: " << summary << '\n';
            if (getenv_bool("ARCHSTREAMER_GST_VERBOSE")) {
                std::cout << "Managed GStreamer pipeline detail: " << description << '\n';
            }
            GError* error = nullptr;
            auto* pipeline = gst_parse_launch(std::string(description).c_str(), &error);
            if (error != nullptr) {
                const std::string reason = std::string("parse failed: ") + error->message;
                g_error_free(error);
                if (pipeline == nullptr) {
                    return fail(reason);
                }
                std::cerr << "Managed GStreamer video warning: " << reason << '\n';
            }
            if (pipeline == nullptr) {
                return fail("parse returned no pipeline");
            }

            auto* bus = gst_element_get_bus(pipeline);
            const auto ret = gst_element_set_state(pipeline, GST_STATE_PLAYING);
            if (ret == GST_STATE_CHANGE_FAILURE) {
                std::string reason = "state change to PLAYING failed";
                if (bus != nullptr) {
                    GstMessage* msg = gst_bus_timed_pop_filtered(
                        bus,
                        2 * GST_SECOND,
                        static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS));
                    if (msg != nullptr) {
                        if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
                            reason += ": " + gst_message_error_string(msg);
                        } else if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_EOS) {
                            reason += ": pipeline posted EOS during startup";
                        }
                        gst_message_unref(msg);
                    }
                }
                if (bus != nullptr) {
                    gst_object_unref(bus);
                }
                gst_element_set_state(pipeline, GST_STATE_NULL);
                gst_object_unref(pipeline);
                return fail(reason);
            }

            if (bus != nullptr) {
                GstMessage* msg = gst_bus_timed_pop_filtered(
                    bus,
                    500 * GST_MSECOND,
                    static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS));
                if (msg != nullptr) {
                    if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
                        const auto reason = gst_message_error_string(msg);
                        gst_message_unref(msg);
                        gst_object_unref(bus);
                        gst_element_set_state(pipeline, GST_STATE_NULL);
                        gst_object_unref(pipeline);
                        return fail(reason);
                    }
                    if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_EOS) {
                        gst_message_unref(msg);
                        gst_object_unref(bus);
                        gst_element_set_state(pipeline, GST_STATE_NULL);
                        gst_object_unref(pipeline);
                        return fail("pipeline posted EOS during startup");
                    }
                    gst_message_unref(msg);
                    gst_object_unref(bus);
                    gst_element_set_state(pipeline, GST_STATE_NULL);
                    gst_object_unref(pipeline);
                    return fail("pipeline posted an unexpected startup message");
                }
                gst_object_unref(bus);
            }

            pipeline_ = pipeline;
            start_bus_monitor();
            return true;
        }
#else
        (void)description;
#endif
        return fail("GStreamer libraries are not available in this build");
    }

    void* pipeline_ = nullptr; // GstElement*
    std::string last_error_;
    std::atomic<bool> monitor_running_{false};
    std::thread monitor_;

    void start_bus_monitor() {
#if defined(ARCHSTREAMER_HAS_GST_LIBS)
        monitor_running_ = false;
        if (monitor_.joinable()) {
            monitor_.join();
        }
        auto* pipeline = static_cast<GstElement*>(pipeline_);
        if (pipeline == nullptr) {
            return;
        }
        auto* bus = gst_element_get_bus(pipeline);
        if (bus == nullptr) {
            return;
        }
        monitor_running_ = true;
        monitor_ = std::thread([this, bus] {
            while (monitor_running_.load(std::memory_order_relaxed)) {
                GstMessage* msg = gst_bus_timed_pop_filtered(
                    bus,
                    250 * GST_MSECOND,
                    static_cast<GstMessageType>(
                        GST_MESSAGE_ERROR |
                        GST_MESSAGE_EOS |
                        GST_MESSAGE_WARNING));
                if (msg == nullptr) {
                    continue;
                }
                if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
                    std::cerr
                        << "Managed GStreamer video runtime error: "
                        << gst_message_error_string(msg) << '\n';
                } else if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_WARNING) {
                    GError* warning = nullptr;
                    gchar* debug = nullptr;
                    gst_message_parse_warning(msg, &warning, &debug);
                    std::cerr << "Managed GStreamer video warning";
                    if (GST_OBJECT_NAME(msg->src) != nullptr) {
                        std::cerr << " at " << GST_OBJECT_NAME(msg->src);
                    }
                    std::cerr << ": "
                              << (warning != nullptr ? warning->message : "unknown warning");
                    if (debug != nullptr) {
                        std::cerr << " | debug: " << debug;
                    }
                    std::cerr << '\n';
                    if (warning != nullptr) {
                        g_error_free(warning);
                    }
                    if (debug != nullptr) {
                        g_free(debug);
                    }
                } else if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_EOS) {
                    std::cerr << "Managed GStreamer video runtime EOS\n";
                }
                gst_message_unref(msg);
            }
            gst_object_unref(bus);
        });
#endif
    }
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
    const bool legacy_nvenc = gst_element_available("nvh264enc");
    const bool managed_nvenc_device_select =
        nvenc_cuda_device_id >= 0 &&
        gst_element_available("nvautogpuh264enc") &&
        gst_element_has_writable_property("nvautogpuh264enc", "cuda-device-id");
    return H264BranchOptions{
        managed_nvenc_device_select || legacy_nvenc,
        managed_nvenc_device_select,
        nvenc_cuda_device_id,
    };
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

    restart_pipeline();
    const bool ok = shared_pipeline_running();
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
    stop_shared_pipeline();
    destinations_.clear();
    display_.clear();
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
            stop_shared_pipeline();
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

void GStreamerVideoFanout::stop_shared_pipeline() {
    if (shared_pipeline_) {
        shared_pipeline_->stop();
    }
}

bool GStreamerVideoFanout::shared_pipeline_running() const {
    return shared_pipeline_ != nullptr && shared_pipeline_->running();
}

void GStreamerVideoFanout::restart_pipeline() {
    stop_shared_pipeline();

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
        return;
    }

    const auto restart_started = std::chrono::steady_clock::now();
    auto log_restart_step = [&](std::string_view step) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - restart_started);
        std::cout << "Video pipeline restart +" << elapsed.count() << "ms: " << step << '\n';
    };

    // Crash leftovers (e.g. black ximagesrc) can keep publishing on the same RTP ports.
    for (const auto& destination : destinations_) {
        if (!pipeline_running(destination.dedicated)) {
            terminate_gst_multiudpsink_on_port(destination.port); 
        }
    }
    log_restart_step("stale destination ports cleared");
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

    const bool legacy_nvenc = gst_element_available("nvh264enc");
    log_restart_step("legacy nvenc probe complete");
    const bool managed_nvenc_device_select =
        nvenc_cuda_device_id_ >= 0 &&
        gst_element_available("nvautogpuh264enc") &&
        gst_element_has_writable_property("nvautogpuh264enc", "cuda-device-id");
    log_restart_step("managed nvenc probe complete");

    const SharedVideoSource source{
        source_kind_ == SourceKind::PipeWire
            ? SharedVideoSource::Kind::PipeWire
            : SharedVideoSource::Kind::X11,
        source_kind_ == SourceKind::PipeWire ? pipewire_target_.path : display_,
        source_kind_ == SourceKind::PipeWire ? pipewire_target_.target_object : std::string{},
    };

    const H264BranchOptions options{
        managed_nvenc_device_select || legacy_nvenc,
        managed_nvenc_device_select,
        nvenc_cuda_device_id_,
    };

    shared_pipeline_ = start_selected_video_pipeline(
        source,
        branches,
        options,
        VideoPipelineUse::Shared);
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

void GStreamerAudioFanout::restart() {
    restart_pipeline();
}

void GStreamerAudioFanout::restart_pipeline() {
    process_.stop();
    if (destinations_.empty()) {
        return;
    }
    for (const auto& destination : destinations_) {
        terminate_gst_multiudpsink_on_port(destination.port);
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
    // Pulse-style "*.monitor" names (e.g. archstreamer.monitor) must use pulsesrc.
    // pipewiresrc used to fall through to @DEFAULT_MONITOR@ whenever the name
    // contained ".monitor", which captured the wrong sink (HDMI/etc.) while
    // RetroArch played into the silent null sink — late/missing client audio and
    // audio_sync pacing stalls on the host (Space=FF looked like a "video unstick").
    const bool pulse_monitor =
        !source_.empty() && source_.size() > 8 &&
        source_.compare(source_.size() - 8, 8, ".monitor") == 0;
    if (backend_ == AudioCaptureBackend::Pulse || pulse_monitor) {
        args.push_back("pulsesrc");
        args.push_back("client-name=ArchStreamer");
        args.push_back("do-timestamp=true");
        // Keep capture latency tight so the null-sink monitor stays awake and
        // RetroArch's audio_sync clock does not build a multi-second backlog.
        args.push_back("buffer-time=80000");
        args.push_back("latency-time=20000");
        args.push_back("provide-clock=false");
        if (!source_.empty()) {
            args.push_back("device=" + source_);
        }
    } else {
        args.push_back("pipewiresrc");
        args.push_back("client-name=ArchStreamer");
        args.push_back("do-timestamp=true");
        if (!source_.empty()) {
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
    const auto destination = HostMediaDestination{client_id, destination_host};
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
