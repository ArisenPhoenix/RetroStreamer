#include "host/video/gst/gst_video_command.hpp"

#include <array>
#include <ostream>

namespace archstreamer {
namespace {

struct VerbName {
    GstVideoCommandVerb verb;
    const char* name;
};

struct SourceName {
    GstVideoSourceKind kind;
    const char* name;
};

struct EventName {
    GstVideoEventKind kind;
    const char* name;
};

constexpr std::array<VerbName, 9> kVerbs{{
    {GstVideoCommandVerb::Start, "start"},
    {GstVideoCommandVerb::Stop, "stop"},
    {GstVideoCommandVerb::Reconfigure, "reconfigure"},
    {GstVideoCommandVerb::ApplyLayout, "apply-layout"},
    {GstVideoCommandVerb::AddSink, "add-sink"},
    {GstVideoCommandVerb::RemoveSink, "remove-sink"},
    {GstVideoCommandVerb::StartStaging, "start-staging"},
    {GstVideoCommandVerb::StopStaging, "stop-staging"},
    {GstVideoCommandVerb::Status, "status"},
}};

constexpr std::array<SourceName, 2> kSources{{
    {GstVideoSourceKind::X11, "x11"},
    {GstVideoSourceKind::PipeWire, "pipewire"},
}};

constexpr std::array<EventName, 7> kEvents{{
    {GstVideoEventKind::Ready, "ready"},
    {GstVideoEventKind::Failed, "failed"},
    {GstVideoEventKind::Error, "error"},
    {GstVideoEventKind::Warning, "warning"},
    {GstVideoEventKind::Eos, "eos"},
    {GstVideoEventKind::Stopped, "stopped"},
    {GstVideoEventKind::Status, "status"},
}};

} // namespace

const char* gst_video_command_verb_name(GstVideoCommandVerb verb) {
    for (const auto& entry : kVerbs) {
        if (entry.verb == verb) {
            return entry.name;
        }
    }
    return "stop";
}

bool gst_video_command_verb_from_name(std::string_view name, GstVideoCommandVerb& verb) {
    for (const auto& entry : kVerbs) {
        if (name == entry.name) {
            verb = entry.verb;
            return true;
        }
    }
    return false;
}

const char* gst_video_source_kind_name(GstVideoSourceKind kind) {
    for (const auto& entry : kSources) {
        if (entry.kind == kind) {
            return entry.name;
        }
    }
    return "x11";
}

bool gst_video_source_kind_from_name(std::string_view name, GstVideoSourceKind& kind) {
    for (const auto& entry : kSources) {
        if (name == entry.name) {
            kind = entry.kind;
            return true;
        }
    }
    return false;
}

const char* gst_video_event_kind_name(GstVideoEventKind kind) {
    for (const auto& entry : kEvents) {
        if (entry.kind == kind) {
            return entry.name;
        }
    }
    return "failed";
}

bool gst_video_event_kind_from_name(std::string_view name, GstVideoEventKind& kind) {
    for (const auto& entry : kEvents) {
        if (name == entry.name) {
            kind = entry.kind;
            return true;
        }
    }
    return false;
}

void gst_video_print_command_help(std::ostream& out) {
    out
        << "archstreamer_gst_worker commands (MediaServer video ops today):\n"
        << "  start          fanout start / start_pipewire / restart_pipeline\n"
        << "                 raw field 'description' = in-process parse-launch string\n"
        << "                 graph fields = worker-owned source + branches + cuda\n"
        << "  stop           fanout stop (shared + staging)\n"
        << "  reconfigure    reconfigure_shared — keep sinks, replace encode, relaunch\n"
        << "  apply-layout   apply_branch_layout — replace branches, relaunch\n"
        << "  add-sink       add() — extra host:port (+ optional encode), relaunch\n"
        << "  remove-sink    stop_client — drop host:port, relaunch\n"
        << "  start-staging  begin_tier_cutover (second pipeline; unused for quality)\n"
        << "  stop-staging   abort_tier_cutover\n"
        << "  status         running / staging stand-in\n"
        << "\n"
        << "Gaps this surface makes explicit:\n"
        << "  - reconfigure/layout/sinks need a graph start, not a raw description\n"
        << "  - every mutate still relaunches parse-launch (no live bitrate/tee yet)\n"
        << "  - staging is a second pipeline, not a cutover of the trunk\n";
}

} // namespace archstreamer
