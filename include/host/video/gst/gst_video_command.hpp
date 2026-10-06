#pragma once

#include <cstdint>
#include <iosfwd>
#include <string>
#include <string_view>
#include <vector>

namespace archstreamer {

/**
 * Verbs that already exist on GStreamerVideoFanout / MediaServer video.
 * Slice 1 implements them with today's semantics: most mutate-and-relaunch
 * the parse-launch graph. Live pad surgery is not done yet — that is the
 * gap these names make visible.
 *
 *   start          start / start_pipewire / restart_pipeline
 *   stop           stop (shared + staging)
 *   reconfigure    reconfigure_shared — same sinks, new encode, relaunch
 *   apply-layout   apply_branch_layout — per-encode branches, relaunch
 *   add-sink       add() — extra UDP client, relaunch
 *   remove-sink    stop_client / remove_client — relaunch
 *   start-staging  begin_tier_cutover (second pipeline; unused for quality)
 *   stop-staging   abort_tier_cutover
 *   status         running / staging stand-in
 *
 * Raw [description] start is the in-process MediaServer escape hatch
 * (host still builds the parse-launch string). Graph start is what the
 * worker needs so reconfigure/layout/sinks have state to mutate.
 */
enum class GstVideoCommandVerb {
    Start,
    Stop,
    Reconfigure,
    ApplyLayout,
    AddSink,
    RemoveSink,
    StartStaging,
    StopStaging,
    Status,
};

enum class GstVideoSourceKind {
    X11,
    PipeWire,
};

struct GstVideoSource {
    GstVideoSourceKind kind = GstVideoSourceKind::X11;
    std::string value;
    std::string target_object;
};

/** Same knobs as VideoEncodeSettings — kept local so this library does not pull protocol.hpp. */
struct GstVideoEncode {
    std::uint16_t bitrate_kbps = 1500;
    std::uint8_t framerate = 60;
    std::uint16_t key_int_max = 30;
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    std::uint8_t queue_buffers = 1;
    bool nvenc_high_quality = false;
};

struct GstVideoSink {
    std::string host;
    std::uint16_t port = 0;
};

struct GstVideoBranch {
    GstVideoEncode encode;
    std::vector<GstVideoSink> sinks;
};

struct GstVideoGraph {
    GstVideoSource source;
    int nvenc_cuda_device_id = -1;
    std::vector<GstVideoBranch> branches;
};

struct GstVideoCommand {
    GstVideoCommandVerb verb = GstVideoCommandVerb::Stop;
    std::string summary;
    /** Raw gst_parse_launch string (MediaServer in-process path). */
    std::string description;
    bool has_graph = false;
    GstVideoGraph graph;
    bool has_encode = false;
    GstVideoEncode encode;
    bool has_sink = false;
    GstVideoSink sink;
};

enum class GstVideoEventKind {
    Ready,
    Failed,
    Error,
    Warning,
    Eos,
    Stopped,
    Status,
};

struct GstVideoEvent {
    GstVideoEventKind kind = GstVideoEventKind::Failed;
    std::string message;
};

inline GstVideoCommand gst_video_start_command(
    std::string description,
    std::string summary = {}) {
    GstVideoCommand command;
    command.verb = GstVideoCommandVerb::Start;
    command.summary = std::move(summary);
    command.description = std::move(description);
    return command;
}

inline GstVideoCommand gst_video_graph_command(
    GstVideoCommandVerb verb,
    GstVideoGraph graph,
    std::string summary = {}) {
    GstVideoCommand command;
    command.verb = verb;
    command.summary = std::move(summary);
    command.has_graph = true;
    command.graph = std::move(graph);
    return command;
}

inline GstVideoCommand gst_video_stop_command() {
    return GstVideoCommand{};
}

inline GstVideoCommand gst_video_status_command() {
    GstVideoCommand command;
    command.verb = GstVideoCommandVerb::Status;
    return command;
}

inline GstVideoCommand gst_video_stop_staging_command() {
    GstVideoCommand command;
    command.verb = GstVideoCommandVerb::StopStaging;
    return command;
}

const char* gst_video_command_verb_name(GstVideoCommandVerb verb);
bool gst_video_command_verb_from_name(std::string_view name, GstVideoCommandVerb& verb);

const char* gst_video_source_kind_name(GstVideoSourceKind kind);
bool gst_video_source_kind_from_name(std::string_view name, GstVideoSourceKind& kind);

const char* gst_video_event_kind_name(GstVideoEventKind kind);
bool gst_video_event_kind_from_name(std::string_view name, GstVideoEventKind& kind);

void gst_video_print_command_help(std::ostream& out);

} // namespace archstreamer
