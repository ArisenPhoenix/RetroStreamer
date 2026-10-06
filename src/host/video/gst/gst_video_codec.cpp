#include "host/video/gst/gst_video_codec.hpp"
#include "host/video/gst/gst_video_pipeline.hpp"

#include <cctype>
#include <istream>
#include <limits>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

namespace archstreamer {
namespace {

constexpr std::size_t kMaxPayloadBytes = 16u * 1024u * 1024u;

bool read_line(std::istream& in, std::string& line) {
    line.clear();
    if (!std::getline(in, line)) {
        return false;
    }
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    return true;
}

bool parse_size(std::string_view text, std::size_t& size) {
    if (text.empty()) {
        return false;
    }
    std::size_t value = 0;
    for (const char ch : text) {
        if (!std::isdigit(static_cast<unsigned char>(ch))) {
            return false;
        }
        const std::size_t digit = static_cast<std::size_t>(ch - '0');
        if (value > (std::numeric_limits<std::size_t>::max() - digit) / 10) {
            return false;
        }
        value = value * 10 + digit;
    }
    if (value > kMaxPayloadBytes) {
        return false;
    }
    size = value;
    return true;
}

bool read_payload(std::istream& in, std::string& payload) {
    std::string len_line;
    if (!read_line(in, len_line)) {
        return false;
    }
    std::size_t size = 0;
    if (!parse_size(len_line, size)) {
        return false;
    }
    payload.assign(size, '\0');
    if (size == 0) {
        return true;
    }
    return static_cast<bool>(in.read(payload.data(), static_cast<std::streamsize>(size)));
}

bool write_payload(std::ostream& out, std::string_view payload) {
    out << payload.size() << '\n';
    out.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    return static_cast<bool>(out);
}

void add_field(
    std::vector<std::pair<std::string, std::string>>& fields,
    std::string name,
    std::string value) {
    fields.emplace_back(std::move(name), std::move(value));
}

} // namespace

bool write_gst_video_command(std::ostream& out, const GstVideoCommand& command) {
    std::vector<std::pair<std::string, std::string>> fields;
    if (!command.summary.empty()) {
        add_field(fields, "summary", command.summary);
    }
    if (!command.description.empty()) {
        add_field(fields, "description", command.description);
    }
    if (command.has_graph) {
        add_field(fields, "graph", gst_video_encode_graph(command.graph));
    }
    if (command.has_encode) {
        add_field(fields, "encode", gst_video_encode_csv(command.encode));
    }
    if (command.has_sink) {
        add_field(fields, "sink_host", command.sink.host);
        add_field(fields, "sink_port", std::to_string(command.sink.port));
    }

    out << gst_video_command_verb_name(command.verb) << '\n'
        << fields.size() << '\n';
    for (const auto& [name, value] : fields) {
        out << name << '\n';
        if (!write_payload(out, value)) {
            return false;
        }
    }
    out.flush();
    return static_cast<bool>(out);
}

bool read_gst_video_command(std::istream& in, GstVideoCommand& command) {
    std::string verb_line;
    if (!read_line(in, verb_line)) {
        return false;
    }
    GstVideoCommandVerb verb = GstVideoCommandVerb::Stop;
    if (!gst_video_command_verb_from_name(verb_line, verb)) {
        return false;
    }
    std::string count_line;
    if (!read_line(in, count_line)) {
        return false;
    }
    std::size_t field_count = 0;
    if (!parse_size(count_line, field_count) || field_count > 32) {
        return false;
    }

    command = GstVideoCommand{};
    command.verb = verb;
    for (std::size_t i = 0; i < field_count; ++i) {
        std::string name;
        std::string value;
        if (!read_line(in, name) || !read_payload(in, value)) {
            return false;
        }
        if (name == "summary") {
            command.summary = std::move(value);
        } else if (name == "description") {
            command.description = std::move(value);
        } else if (name == "graph") {
            if (!gst_video_decode_graph(value, command.graph)) {
                return false;
            }
            command.has_graph = true;
        } else if (name == "encode") {
            if (!gst_video_decode_csv(value, command.encode)) {
                return false;
            }
            command.has_encode = true;
        } else if (name == "sink_host") {
            command.sink.host = std::move(value);
            command.has_sink = true;
        } else if (name == "sink_port") {
            std::size_t port = 0;
            if (!parse_size(value, port) || port > 65535) {
                return false;
            }
            command.sink.port = static_cast<std::uint16_t>(port);
            command.has_sink = true;
        }
    }
    return true;
}

bool write_gst_video_event(std::ostream& out, const GstVideoEvent& event) {
    out << gst_video_event_kind_name(event.kind) << '\n';
    if (!write_payload(out, event.message)) {
        return false;
    }
    out.flush();
    return static_cast<bool>(out);
}

bool read_gst_video_event(std::istream& in, GstVideoEvent& event) {
    std::string kind_line;
    if (!read_line(in, kind_line)) {
        return false;
    }
    GstVideoEventKind kind = GstVideoEventKind::Failed;
    if (!gst_video_event_kind_from_name(kind_line, kind)) {
        return false;
    }
    event.kind = kind;
    return read_payload(in, event.message);
}

} // namespace archstreamer
