#include "host/video/gst/gst_video_engine.hpp"
#include "host/video/gst/gst_video_pipeline.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>
#include <utility>

#include <gst/gst.h>
#include <unistd.h>

namespace archstreamer {
namespace {

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

bool gst_verbose() {
    const char* value = std::getenv("ARCHSTREAMER_GST_VERBOSE");
    if (value == nullptr) {
        return false;
    }
    return std::strcmp(value, "1") == 0 ||
        std::strcmp(value, "true") == 0 ||
        std::strcmp(value, "TRUE") == 0 ||
        std::strcmp(value, "yes") == 0 ||
        std::strcmp(value, "on") == 0;
}

std::string gst_message_error_string(GstMessage* msg) {
    GError* gst_error = nullptr;
    gchar* debug = nullptr;
    gst_message_parse_error(msg, &gst_error, &debug);
    std::string reason;
    if (GST_OBJECT_NAME(msg->src) != nullptr) {
        reason += GST_OBJECT_NAME(msg->src);
        reason += ": ";
    }
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

std::string gst_message_warning_string(GstMessage* msg) {
    GError* warning = nullptr;
    gchar* debug = nullptr;
    gst_message_parse_warning(msg, &warning, &debug);
    std::string reason;
    if (GST_OBJECT_NAME(msg->src) != nullptr) {
        reason += "at ";
        reason += GST_OBJECT_NAME(msg->src);
        reason += ": ";
    }
    reason += warning != nullptr ? warning->message : "unknown warning";
    if (debug != nullptr) {
        reason += " | debug: ";
        reason += debug;
    }
    if (warning != nullptr) {
        g_error_free(warning);
    }
    if (debug != nullptr) {
        g_free(debug);
    }
    return reason;
}

GstVideoEvent make_event(GstVideoEventKind kind, std::string message = {}) {
    return GstVideoEvent{kind, std::move(message)};
}

bool encode_equal(const GstVideoEncode& a, const GstVideoEncode& b) {
    return a.bitrate_kbps == b.bitrate_kbps &&
        a.framerate == b.framerate &&
        a.key_int_max == b.key_int_max &&
        a.width == b.width &&
        a.height == b.height &&
        a.queue_buffers == b.queue_buffers &&
        a.nvenc_high_quality == b.nvenc_high_quality;
}

bool sink_equal(const GstVideoSink& a, const GstVideoSink& b) {
    return a.host == b.host && a.port == b.port;
}

} // namespace

class GstVideoEngine::Impl {
public:
    ~Impl() {
        stop_slot(shared_, true);
        stop_slot(staging_, true);
    }

    void set_event_handler(EventHandler handler) {
        std::lock_guard lock(handler_mutex_);
        handler_ = std::move(handler);
    }

    GstVideoEvent execute(const GstVideoCommand& command) {
        switch (command.verb) {
        case GstVideoCommandVerb::Start:
            return start_shared(command);
        case GstVideoCommandVerb::Stop:
            stop_slot(shared_, true);
            stop_slot(staging_, true);
            return make_event(GstVideoEventKind::Stopped);
        case GstVideoCommandVerb::Reconfigure:
            return reconfigure(command);
        case GstVideoCommandVerb::ApplyLayout:
            return apply_layout(command);
        case GstVideoCommandVerb::AddSink:
            return add_sink(command);
        case GstVideoCommandVerb::RemoveSink:
            return remove_sink(command);
        case GstVideoCommandVerb::StartStaging:
            return start_staging(command);
        case GstVideoCommandVerb::StopStaging:
            stop_slot(staging_, true);
            return make_event(GstVideoEventKind::Stopped, "staging");
        case GstVideoCommandVerb::Status:
            return status();
        }
        return make_event(GstVideoEventKind::Failed, "unknown command");
    }

    bool running() const {
        std::lock_guard lock(shared_.pipeline_mutex);
        return slot_running_locked(shared_);
    }

private:
    struct Slot {
        mutable std::mutex pipeline_mutex;
        GstElement* pipeline = nullptr;
        std::atomic<bool> monitor_running{false};
        std::thread monitor;
        bool has_graph = false;
        GstVideoGraph graph;
        std::string raw_description;
        std::string pipeline_id;
    };

    static std::string next_pipeline_id() {
        static std::atomic<unsigned> sequence{0};
        const auto n = sequence.fetch_add(1, std::memory_order_relaxed) + 1;
        return "asv-" + std::to_string(static_cast<long long>(getpid())) + "-" +
            std::to_string(n);
    }

    static void remove_pipeline_record(const std::string& pipeline_id) {
        if (pipeline_id.empty()) {
            return;
        }
        std::error_code error;
        std::filesystem::remove(gst_video_pipeline_record_path(pipeline_id), error);
    }

    static void publish_pipeline_record(const Slot& slot) {
        if (slot.pipeline_id.empty()) {
            return;
        }
        const auto path = gst_video_pipeline_record_path(slot.pipeline_id);
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        std::ofstream out(path, std::ios::trunc);
        if (!out) {
            return;
        }
        out << "id=" << slot.pipeline_id << '\n'
            << "pid=" << getpid() << '\n'
            << "pipeline=" << slot.pipeline_id << '\n'
            << "state=playing\n";
        if (slot.has_graph) {
            out << "source="
                << (slot.graph.source.kind == GstVideoSourceKind::PipeWire ? "pipewire" : "x11")
                << '\n'
                << "path=" << slot.graph.source.value << '\n';
            if (!slot.graph.source.target_object.empty()) {
                out << "target-object=" << slot.graph.source.target_object << '\n';
            }
        }
    }

    static bool slot_running_locked(const Slot& slot) {
        if (slot.pipeline == nullptr) {
            return false;
        }
        GstState state = GST_STATE_NULL;
        gst_element_get_state(slot.pipeline, &state, nullptr, 0);
        return state == GST_STATE_PLAYING || state == GST_STATE_PAUSED;
    }

    void emit(const GstVideoEvent& event) {
        EventHandler handler;
        {
            std::lock_guard lock(handler_mutex_);
            handler = handler_;
        }
        if (handler) {
            handler(event);
        }
    }

    GstVideoEvent require_shared_graph(const char* verb) {
        if (!shared_.has_graph) {
            std::ostringstream out;
            out << verb
                << " needs a graph start (raw parse-launch has no sinks/encodes to mutate)";
            return make_event(GstVideoEventKind::Failed, out.str());
        }
        return make_event(GstVideoEventKind::Ready);
    }

    GstVideoEvent relaunch_shared(std::string summary) {
        GstVideoCommand launch;
        launch.verb = GstVideoCommandVerb::Start;
        launch.summary = std::move(summary);
        launch.has_graph = true;
        launch.graph = shared_.graph;
        return start_slot(shared_, launch, true);
    }

    GstVideoEvent start_shared(const GstVideoCommand& command) {
        return start_slot(shared_, command, command.has_graph);
    }

    GstVideoEvent start_staging(const GstVideoCommand& command) {
        if (!command.has_graph && command.description.empty()) {
            return make_event(
                GstVideoEventKind::Failed,
                "start-staging needs a graph or description");
        }
        return start_slot(staging_, command, command.has_graph);
    }

    GstVideoEvent start_slot(Slot& slot, const GstVideoCommand& command, bool graph_mode) {
        if (graph_mode) {
            if (command.graph.branches.empty()) {
                return make_event(GstVideoEventKind::Failed, "graph has no branches");
            }
            if (command.graph.source.kind == GstVideoSourceKind::X11 &&
                command.graph.source.value.empty()) {
                return make_event(GstVideoEventKind::Failed, "x11 source display is empty");
            }
            if (command.graph.source.kind == GstVideoSourceKind::PipeWire &&
                command.graph.source.value.empty() &&
                command.graph.source.target_object.empty()) {
                return make_event(GstVideoEventKind::Failed, "pipewire source path is empty");
            }
            const auto options = gst_video_probe_h264(command.graph.nvenc_cuda_device_id);
            auto selectors = gst_video_source_selectors(command.graph.source);
            if (selectors.empty()) {
                selectors.emplace_back("display-name", command.graph.source.value);
            }
            constexpr int kAttempts = 8;
            const auto delay = std::chrono::milliseconds(250);
            std::string errors;
            for (int attempt = 1; attempt <= kAttempts; ++attempt) {
                if (attempt > 1) {
                    std::this_thread::sleep_for(delay);
                }
                for (const auto& [property, value] : selectors) {
                    const auto description = gst_video_build_description(
                        command.graph, property, value, options);
                    GstVideoCommand raw = command;
                    raw.description = description;
                    raw.has_graph = false;
                    const auto reply = play_description(slot, raw);
                    if (reply.kind == GstVideoEventKind::Ready) {
                        slot.has_graph = true;
                        slot.graph = command.graph;
                        slot.raw_description.clear();
                        publish_pipeline_record(slot);
                        return reply;
                    }
                    if (!errors.empty()) {
                        errors += " | ";
                    }
                    errors += property + "=" + value + ": " + reply.message;
                }
                if (command.graph.source.kind != GstVideoSourceKind::PipeWire) {
                    break;
                }
            }
            return make_event(GstVideoEventKind::Failed, errors);
        }

        const auto reply = play_description(slot, command);
        if (reply.kind == GstVideoEventKind::Ready) {
            slot.has_graph = false;
            slot.graph = {};
            slot.raw_description = command.description;
        }
        return reply;
    }

    GstVideoEvent play_description(Slot& slot, const GstVideoCommand& command) {
        stop_slot(slot, false);
        if (command.description.empty()) {
            return make_event(GstVideoEventKind::Failed, "empty pipeline description");
        }

        ensure_gst_initialized();
        if (!g_gst_available) {
            return make_event(GstVideoEventKind::Failed, "GStreamer libraries are not available");
        }

        if (gst_verbose()) {
            std::cerr << "Gst video pipeline detail: " << command.description << '\n';
        }

        GError* error = nullptr;
        auto* pipeline = gst_parse_launch(command.description.c_str(), &error);
        if (error != nullptr) {
            const std::string reason = std::string("parse failed: ") + error->message;
            g_error_free(error);
            if (pipeline == nullptr) {
                return make_event(GstVideoEventKind::Failed, reason);
            }
            emit(make_event(GstVideoEventKind::Warning, reason));
        }
        if (pipeline == nullptr) {
            return make_event(GstVideoEventKind::Failed, "parse returned no pipeline");
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
                        reason += ": ";
                        reason += gst_message_error_string(msg);
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
            return make_event(GstVideoEventKind::Failed, reason);
        }

        if (bus != nullptr) {
            GstMessage* msg = gst_bus_timed_pop_filtered(
                bus,
                500 * GST_MSECOND,
                static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS));
            if (msg != nullptr) {
                std::string reason = "pipeline posted an unexpected startup message";
                if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
                    reason = gst_message_error_string(msg);
                } else if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_EOS) {
                    reason = "pipeline posted EOS during startup";
                }
                gst_message_unref(msg);
                gst_object_unref(bus);
                gst_element_set_state(pipeline, GST_STATE_NULL);
                gst_object_unref(pipeline);
                return make_event(GstVideoEventKind::Failed, reason);
            }
            gst_object_unref(bus);
        }

        {
            std::lock_guard lock(slot.pipeline_mutex);
            slot.pipeline = pipeline;
        }
        start_bus_monitor(slot);
        if (slot.pipeline_id.empty()) {
            slot.pipeline_id = next_pipeline_id();
        }
        gst_element_set_name(pipeline, slot.pipeline_id.c_str());
        publish_pipeline_record(slot);
        std::ostringstream ready;
        ready << "id=" << slot.pipeline_id << '\n' << command.summary;
        return make_event(GstVideoEventKind::Ready, ready.str());
    }

    GstVideoEvent reconfigure(const GstVideoCommand& command) {
        const auto check = require_shared_graph("reconfigure");
        if (check.kind != GstVideoEventKind::Ready) {
            return check;
        }
        if (!command.has_encode) {
            return make_event(GstVideoEventKind::Failed, "reconfigure needs encode");
        }
        for (auto& branch : shared_.graph.branches) {
            branch.encode = command.encode;
        }
        return relaunch_shared(command.summary.empty() ? "reconfigure" : command.summary);
    }

    GstVideoEvent apply_layout(const GstVideoCommand& command) {
        const auto check = require_shared_graph("apply-layout");
        if (check.kind != GstVideoEventKind::Ready) {
            return check;
        }
        if (!command.has_graph || command.graph.branches.empty()) {
            return make_event(GstVideoEventKind::Failed, "apply-layout needs graph branches");
        }
        if (command.graph.source.value.empty() && command.graph.source.target_object.empty()) {
            auto next = command.graph;
            next.source = shared_.graph.source;
            if (next.nvenc_cuda_device_id < 0) {
                next.nvenc_cuda_device_id = shared_.graph.nvenc_cuda_device_id;
            }
            shared_.graph = std::move(next);
        } else {
            shared_.graph = command.graph;
        }
        return relaunch_shared(command.summary.empty() ? "apply-layout" : command.summary);
    }

    GstVideoEvent add_sink(const GstVideoCommand& command) {
        const auto check = require_shared_graph("add-sink");
        if (check.kind != GstVideoEventKind::Ready) {
            return check;
        }
        if (!command.has_sink || command.sink.host.empty()) {
            return make_event(GstVideoEventKind::Failed, "add-sink needs sink_host and sink_port");
        }
        GstVideoEncode encode = command.has_encode
            ? command.encode
            : shared_.graph.branches.front().encode;
        bool merged = false;
        for (auto& branch : shared_.graph.branches) {
            if (encode_equal(branch.encode, encode)) {
                for (const auto& existing : branch.sinks) {
                    if (sink_equal(existing, command.sink)) {
                        return relaunch_shared(command.summary.empty() ? "add-sink" : command.summary);
                    }
                }
                branch.sinks.push_back(command.sink);
                merged = true;
                break;
            }
        }
        if (!merged) {
            shared_.graph.branches.push_back(GstVideoBranch{encode, {command.sink}});
        }
        return relaunch_shared(command.summary.empty() ? "add-sink" : command.summary);
    }

    GstVideoEvent remove_sink(const GstVideoCommand& command) {
        const auto check = require_shared_graph("remove-sink");
        if (check.kind != GstVideoEventKind::Ready) {
            return check;
        }
        if (!command.has_sink) {
            return make_event(GstVideoEventKind::Failed, "remove-sink needs sink_host and sink_port");
        }
        for (auto branch = shared_.graph.branches.begin();
             branch != shared_.graph.branches.end();) {
            auto& sinks = branch->sinks;
            sinks.erase(
                std::remove_if(
                    sinks.begin(),
                    sinks.end(),
                    [&](const GstVideoSink& sink) { return sink_equal(sink, command.sink); }),
                sinks.end());
            if (sinks.empty()) {
                branch = shared_.graph.branches.erase(branch);
            } else {
                ++branch;
            }
        }
        if (shared_.graph.branches.empty()) {
            stop_slot(shared_, true);
            return make_event(GstVideoEventKind::Stopped, "no sinks left");
        }
        return relaunch_shared(command.summary.empty() ? "remove-sink" : command.summary);
    }

    GstVideoEvent status() const {
        std::ostringstream out;
        bool shared_running = false;
        bool staging_running = false;
        {
            std::lock_guard lock(shared_.pipeline_mutex);
            shared_running = slot_running_locked(shared_);
        }
        {
            std::lock_guard lock(staging_.pipeline_mutex);
            staging_running = slot_running_locked(staging_);
        }
        if (!shared_.pipeline_id.empty()) {
            out << "id=" << shared_.pipeline_id << '\n';
        }
        out << "shared=" << (shared_running ? "playing" : "stopped");
        if (shared_running) {
            std::lock_guard lock(shared_.pipeline_mutex);
            if (shared_.pipeline != nullptr) {
                gchar* name = gst_element_get_name(shared_.pipeline);
                if (name != nullptr) {
                    out << " pipeline=" << name;
                    g_free(name);
                }
            }
        }
        if (shared_.has_graph) {
            out << " graph=" << gst_video_graph_summary(shared_.graph);
        } else if (!shared_.raw_description.empty()) {
            out << " raw-description";
        }
        out << " staging=" << (staging_running ? "playing" : "stopped");
        return make_event(GstVideoEventKind::Status, out.str());
    }

    void stop_slot(Slot& slot, bool retire_id) {
        slot.monitor_running = false;
        if (slot.monitor.joinable()) {
            slot.monitor.join();
        }
        GstElement* pipeline = nullptr;
        {
            std::lock_guard lock(slot.pipeline_mutex);
            pipeline = slot.pipeline;
            slot.pipeline = nullptr;
        }
        if (pipeline != nullptr) {
            gst_element_set_state(pipeline, GST_STATE_NULL);
            gst_element_get_state(pipeline, nullptr, nullptr, 2 * GST_SECOND);
            gst_object_unref(pipeline);
        }
        remove_pipeline_record(slot.pipeline_id);
        if (retire_id) {
            slot.pipeline_id.clear();
            slot.has_graph = false;
            slot.raw_description.clear();
        }
    }

    void start_bus_monitor(Slot& slot) {
        slot.monitor_running = false;
        if (slot.monitor.joinable()) {
            slot.monitor.join();
        }
        GstElement* pipeline = nullptr;
        {
            std::lock_guard lock(slot.pipeline_mutex);
            pipeline = slot.pipeline;
        }
        if (pipeline == nullptr) {
            return;
        }
        auto* bus = gst_element_get_bus(pipeline);
        if (bus == nullptr) {
            return;
        }
        slot.monitor_running = true;
        slot.monitor = std::thread([this, &slot, bus] {
            while (slot.monitor_running.load(std::memory_order_relaxed)) {
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
                    emit(make_event(GstVideoEventKind::Error, gst_message_error_string(msg)));
                } else if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_WARNING) {
                    emit(make_event(GstVideoEventKind::Warning, gst_message_warning_string(msg)));
                } else if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_EOS) {
                    emit(make_event(GstVideoEventKind::Eos));
                }
                gst_message_unref(msg);
            }
            gst_object_unref(bus);
        });
    }

    EventHandler handler_;
    std::mutex handler_mutex_;
    Slot shared_;
    Slot staging_;
};

GstVideoEngine::GstVideoEngine() : impl_(std::make_unique<Impl>()) {}

GstVideoEngine::~GstVideoEngine() = default;

void GstVideoEngine::set_event_handler(EventHandler handler) {
    impl_->set_event_handler(std::move(handler));
}

GstVideoEvent GstVideoEngine::execute(const GstVideoCommand& command) {
    return impl_->execute(command);
}

bool GstVideoEngine::running() const {
    return impl_->running();
}

} // namespace archstreamer
