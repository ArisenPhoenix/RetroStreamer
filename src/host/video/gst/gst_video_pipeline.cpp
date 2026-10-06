#include "host/video/gst/gst_video_pipeline.hpp"

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <unistd.h>

#include <gst/gst.h>

namespace archstreamer {
namespace {

std::once_flag g_gst_once;
bool g_gst_available = false;

void ensure_gst_initialized() {
    std::call_once(g_gst_once, [] {
        GError* error = nullptr;
        g_gst_available = gst_init_check(nullptr, nullptr, &error);
        if (error != nullptr) {
            g_error_free(error);
        }
    });
}

} // namespace

bool gst_video_element_available(const char* element) {
    ensure_gst_initialized();
    if (!g_gst_available || element == nullptr || element[0] == '\0') {
        return false;
    }
    static std::mutex cache_mutex;
    static std::unordered_map<std::string, bool> cache;
    std::lock_guard lock(cache_mutex);
    const std::string key{element};
    if (const auto found = cache.find(key); found != cache.end()) {
        return found->second;
    }
    GstElementFactory* factory = gst_element_factory_find(element);
    const bool available = factory != nullptr;
    if (factory != nullptr) {
        gst_object_unref(factory);
    }
    cache.emplace(key, available);
    return available;
}

bool gst_video_element_has_property(const char* element, const char* property) {
    if (!gst_video_element_available(element) || property == nullptr || property[0] == '\0') {
        return false;
    }
    static std::mutex cache_mutex;
    static std::unordered_map<std::string, bool> cache;
    std::lock_guard lock(cache_mutex);
    const std::string key = std::string(element) + '\0' + property;
    if (const auto found = cache.find(key); found != cache.end()) {
        return found->second;
    }
    GstElement* instance = gst_element_factory_make(element, nullptr);
    bool has = false;
    if (instance != nullptr) {
        has = g_object_class_find_property(G_OBJECT_GET_CLASS(instance), property) != nullptr;
        gst_object_unref(instance);
    }
    cache.emplace(key, has);
    return has;
}

namespace {

std::string gst_parse_property(std::string_view key, std::string_view value) {
    return std::string(key) + "=" + gst_video_parse_quote(value);
}

void append_h264_branch(
    std::ostringstream& desc,
    const GstVideoEncode& settings,
    const std::vector<GstVideoSink>& sinks,
    const GstVideoH264Options& options) {
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
    if (gst_video_element_available("h264parse")) {
        desc << " ! h264parse config-interval=-1";
    }
    std::string clients;
    for (const auto& sink : sinks) {
        if (!clients.empty()) {
            clients.push_back(',');
        }
        clients += sink.host;
        clients.push_back(':');
        clients += std::to_string(sink.port);
    }
    desc
        << " ! rtph264pay mtu=1200 config-interval=-1"
        << " aggregate-mode=zero-latency pt=96"
        << " ! multiudpsink clients=" << clients
        << " sync=false async=false";
}

bool parse_u32(std::string_view text, std::uint32_t& value) {
    if (text.empty()) {
        return false;
    }
    std::uint32_t parsed = 0;
    const auto* begin = text.data();
    const auto* end = begin + text.size();
    const auto result = std::from_chars(begin, end, parsed);
    if (result.ec != std::errc{} || result.ptr != end) {
        return false;
    }
    value = parsed;
    return true;
}

std::vector<std::string_view> split_csv(std::string_view text) {
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= text.size(); ++i) {
        if (i == text.size() || text[i] == ',') {
            parts.push_back(text.substr(start, i - start));
            start = i + 1;
        }
    }
    return parts;
}

} // namespace

std::string gst_video_parse_quote(std::string_view value) {
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

GstVideoH264Options gst_video_probe_h264(int nvenc_cuda_device_id) {
    const bool legacy_nvenc = gst_video_element_available("nvh264enc");
    const bool managed_nvenc_device_select =
        nvenc_cuda_device_id >= 0 &&
        gst_video_element_available("nvautogpuh264enc") &&
        gst_video_element_has_property("nvautogpuh264enc", "cuda-device-id");
    return GstVideoH264Options{
        managed_nvenc_device_select || legacy_nvenc,
        managed_nvenc_device_select,
        nvenc_cuda_device_id,
    };
}

std::vector<std::pair<std::string, std::string>> gst_video_source_selectors(
    const GstVideoSource& source) {
    std::vector<std::pair<std::string, std::string>> selectors;
    auto append = [&](std::string property, const std::string& value) {
        if (!value.empty()) {
            selectors.emplace_back(std::move(property), value);
        }
    };
    if (source.kind == GstVideoSourceKind::PipeWire) {
        append("path", source.value);
        append("target-object", source.target_object);
    }
    return selectors;
}

std::string gst_video_build_description(
    const GstVideoGraph& graph,
    std::string_view selector_property,
    std::string_view selector_value,
    const GstVideoH264Options& options) {
    std::ostringstream desc;
    if (graph.source.kind == GstVideoSourceKind::PipeWire) {
        desc
            << "pipewiresrc " << gst_parse_property(selector_property, selector_value)
            << " do-timestamp=true ! video/x-raw,format=BGRx ! videoconvert";
    } else {
        desc
            << "ximagesrc " << gst_parse_property("display-name", graph.source.value)
            << " use-damage=false show-pointer=false do-timestamp=true ! videoconvert";
    }
    if (graph.branches.size() == 1) {
        desc << " ! ";
        append_h264_branch(desc, graph.branches[0].encode, graph.branches[0].sinks, options);
    } else {
        desc << " ! tee name=t";
        for (const auto& branch : graph.branches) {
            desc << " t. ! ";
            append_h264_branch(desc, branch.encode, branch.sinks, options);
        }
    }
    return desc.str();
}

std::string gst_video_encode_csv(const GstVideoEncode& encode) {
    std::ostringstream out;
    out << encode.bitrate_kbps << ','
        << static_cast<int>(encode.framerate) << ','
        << encode.key_int_max << ','
        << encode.width << ','
        << encode.height << ','
        << static_cast<int>(encode.queue_buffers) << ','
        << (encode.nvenc_high_quality ? 1 : 0);
    return out.str();
}

bool gst_video_decode_csv(std::string_view text, GstVideoEncode& encode) {
    const auto parts = split_csv(text);
    if (parts.size() != 7) {
        return false;
    }
    std::uint32_t bitrate = 0;
    std::uint32_t fps = 0;
    std::uint32_t key = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t queue = 0;
    std::uint32_t hq = 0;
    if (!parse_u32(parts[0], bitrate) ||
        !parse_u32(parts[1], fps) ||
        !parse_u32(parts[2], key) ||
        !parse_u32(parts[3], width) ||
        !parse_u32(parts[4], height) ||
        !parse_u32(parts[5], queue) ||
        !parse_u32(parts[6], hq)) {
        return false;
    }
    encode.bitrate_kbps = static_cast<std::uint16_t>(bitrate);
    encode.framerate = static_cast<std::uint8_t>(fps);
    encode.key_int_max = static_cast<std::uint16_t>(key);
    encode.width = static_cast<std::uint16_t>(width);
    encode.height = static_cast<std::uint16_t>(height);
    encode.queue_buffers = static_cast<std::uint8_t>(queue);
    encode.nvenc_high_quality = hq != 0;
    return true;
}

std::string gst_video_encode_graph(const GstVideoGraph& graph) {
    std::ostringstream out;
    out << gst_video_source_kind_name(graph.source.kind) << '\n'
        << graph.source.value.size() << '\n'
        << graph.source.value
        << graph.source.target_object.size() << '\n'
        << graph.source.target_object
        << graph.nvenc_cuda_device_id << '\n'
        << graph.branches.size() << '\n';
    for (const auto& branch : graph.branches) {
        const auto csv = gst_video_encode_csv(branch.encode);
        out << csv.size() << '\n' << csv
            << branch.sinks.size() << '\n';
        for (const auto& sink : branch.sinks) {
            out << sink.host.size() << '\n' << sink.host
                << sink.port << '\n';
        }
    }
    return out.str();
}

bool gst_video_decode_graph(std::string_view text, GstVideoGraph& graph) {
    auto read_line = [&](std::string& line) {
        const auto pos = text.find('\n');
        if (pos == std::string_view::npos) {
            line = std::string(text);
            text = {};
            return !line.empty();
        }
        line = std::string(text.substr(0, pos));
        text.remove_prefix(pos + 1);
        return true;
    };
    auto read_payload = [&](std::string& payload) {
        std::string len_line;
        if (!read_line(len_line)) {
            return false;
        }
        std::uint32_t size = 0;
        if (!parse_u32(len_line, size) || size > text.size()) {
            return false;
        }
        payload = std::string(text.substr(0, size));
        text.remove_prefix(size);
        return true;
    };

    GstVideoGraph parsed;
    std::string kind_line;
    if (!read_line(kind_line) ||
        !gst_video_source_kind_from_name(kind_line, parsed.source.kind) ||
        !read_payload(parsed.source.value) ||
        !read_payload(parsed.source.target_object)) {
        return false;
    }
    std::string cuda_line;
    std::string branch_count_line;
    if (!read_line(cuda_line) || !read_line(branch_count_line)) {
        return false;
    }
    if (cuda_line == "-1") {
        parsed.nvenc_cuda_device_id = -1;
    } else {
        std::uint32_t cuda = 0;
        if (!parse_u32(cuda_line, cuda)) {
            return false;
        }
        parsed.nvenc_cuda_device_id = static_cast<int>(cuda);
    }
    std::uint32_t branch_count = 0;
    if (!parse_u32(branch_count_line, branch_count) || branch_count > 32) {
        return false;
    }
    parsed.branches.resize(branch_count);
    for (auto& branch : parsed.branches) {
        std::string csv;
        if (!read_payload(csv) || !gst_video_decode_csv(csv, branch.encode)) {
            return false;
        }
        std::string sink_count_line;
        if (!read_line(sink_count_line)) {
            return false;
        }
        std::uint32_t sink_count = 0;
        if (!parse_u32(sink_count_line, sink_count) || sink_count > 64) {
            return false;
        }
        branch.sinks.resize(sink_count);
        for (auto& sink : branch.sinks) {
            if (!read_payload(sink.host)) {
                return false;
            }
            std::string port_line;
            if (!read_line(port_line)) {
                return false;
            }
            std::uint32_t port = 0;
            if (!parse_u32(port_line, port) || port > 65535) {
                return false;
            }
            sink.port = static_cast<std::uint16_t>(port);
        }
    }
    graph = std::move(parsed);
    return true;
}

std::string gst_video_graph_summary(const GstVideoGraph& graph) {
    std::ostringstream out;
    out << gst_video_source_kind_name(graph.source.kind)
        << " " << graph.source.value
        << " cuda=" << graph.nvenc_cuda_device_id
        << " branches=" << graph.branches.size();
    for (const auto& branch : graph.branches) {
        out << " [" << gst_video_encode_csv(branch.encode) << " -> ";
        for (std::size_t i = 0; i < branch.sinks.size(); ++i) {
            if (i != 0) {
                out << ',';
            }
            out << branch.sinks[i].host << ':' << branch.sinks[i].port;
        }
        out << ']';
    }
    return out.str();
}

std::filesystem::path gst_video_pipeline_record_path(std::string_view pipeline_id) {
    std::filesystem::path root;
    if (const char* runtime = std::getenv("XDG_RUNTIME_DIR");
        runtime != nullptr && runtime[0] != '\0') {
        root = std::filesystem::path(runtime) / "archstreamer" / "video";
    } else {
        root = std::filesystem::path("/tmp") /
            ("archstreamer-" + std::to_string(static_cast<long long>(geteuid()))) /
            "video";
    }
    return root / (std::string(pipeline_id) + ".session");
}

std::string gst_video_event_pipeline_id(std::string_view message) {
    constexpr std::string_view prefix = "id=";
    if (message.size() < prefix.size() || message.substr(0, prefix.size()) != prefix) {
        return {};
    }
    const auto end = message.find('\n');
    const auto id = message.substr(prefix.size(), end == std::string_view::npos
        ? std::string_view::npos
        : end - prefix.size());
    if (id.empty() || id.find(' ') != std::string_view::npos) {
        return {};
    }
    return std::string(id);
}

} // namespace archstreamer
