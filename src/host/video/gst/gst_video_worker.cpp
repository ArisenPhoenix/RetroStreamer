#include "host/video/gst/gst_video_codec.hpp"
#include "host/video/gst/gst_video_command.hpp"
#include "host/video/gst/gst_video_engine.hpp"
#include "host/video/gst/gst_video_pipeline.hpp"
#include "host/video/gst/gst_video_worker_client.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>

namespace {

using archstreamer::GstVideoCommand;
using archstreamer::GstVideoCommandVerb;
using archstreamer::GstVideoEncode;
using archstreamer::GstVideoEngine;
using archstreamer::GstVideoEvent;
using archstreamer::GstVideoEventKind;
using archstreamer::GstVideoGraph;
using archstreamer::GstVideoSink;
using archstreamer::GstVideoSourceKind;
using archstreamer::gst_video_decode_graph;
using archstreamer::gst_video_encode_graph;
using archstreamer::gst_video_event_kind_name;
using archstreamer::gst_video_graph_command;
using archstreamer::gst_video_print_command_help;
using archstreamer::gst_video_start_command;
using archstreamer::gst_video_status_command;
using archstreamer::gst_video_stop_command;
using archstreamer::read_gst_video_command;
using archstreamer::read_gst_video_event;
using archstreamer::write_gst_video_command;
using archstreamer::write_gst_video_event;

bool commands_equal(const GstVideoCommand& a, const GstVideoCommand& b) {
    if (a.verb != b.verb || a.summary != b.summary || a.description != b.description ||
        a.has_graph != b.has_graph || a.has_encode != b.has_encode || a.has_sink != b.has_sink) {
        return false;
    }
    if (a.has_graph &&
        gst_video_encode_graph(a.graph) != gst_video_encode_graph(b.graph)) {
        return false;
    }
    if (a.has_encode &&
        (a.encode.bitrate_kbps != b.encode.bitrate_kbps ||
         a.encode.framerate != b.encode.framerate)) {
        return false;
    }
    if (a.has_sink && (a.sink.host != b.sink.host || a.sink.port != b.sink.port)) {
        return false;
    }
    return true;
}

int run_codec_self_test() {
    GstVideoGraph graph;
    graph.source.kind = GstVideoSourceKind::X11;
    graph.source.value = ":99";
    graph.nvenc_cuda_device_id = 1;
    graph.branches.push_back(archstreamer::GstVideoBranch{
        GstVideoEncode{3500, 30, 30, 1280, 720, 1, false},
        {GstVideoSink{"192.168.1.8", 5004}},
    });

    GstVideoCommand start = gst_video_start_command("ximagesrc ! fakesink", "self-test");
    GstVideoCommand graph_start = gst_video_graph_command(
        GstVideoCommandVerb::Start, graph, "graph-start");
    GstVideoCommand reconfigure;
    reconfigure.verb = GstVideoCommandVerb::Reconfigure;
    reconfigure.has_encode = true;
    reconfigure.encode.bitrate_kbps = 2500;
    GstVideoCommand add;
    add.verb = GstVideoCommandVerb::AddSink;
    add.has_sink = true;
    add.sink = {"10.0.0.4", 5005};
    GstVideoCommand stop = gst_video_stop_command();
    GstVideoCommand status = gst_video_status_command();
    GstVideoEvent ready{GstVideoEventKind::Ready, "ok"};
    GstVideoEvent failed{GstVideoEventKind::Failed, "nope\nwith newline"};

    std::ostringstream out;
    if (!write_gst_video_command(out, start) ||
        !write_gst_video_command(out, graph_start) ||
        !write_gst_video_command(out, reconfigure) ||
        !write_gst_video_command(out, add) ||
        !write_gst_video_command(out, stop) ||
        !write_gst_video_command(out, status) ||
        !write_gst_video_event(out, ready) ||
        !write_gst_video_event(out, failed)) {
        std::cerr << "gst worker codec self-test: write failed\n";
        return 1;
    }

    std::istringstream in(out.str());
    GstVideoCommand start_round;
    GstVideoCommand graph_round;
    GstVideoCommand reconfigure_round;
    GstVideoCommand add_round;
    GstVideoCommand stop_round;
    GstVideoCommand status_round;
    GstVideoEvent ready_round;
    GstVideoEvent failed_round;
    if (!read_gst_video_command(in, start_round) ||
        !read_gst_video_command(in, graph_round) ||
        !read_gst_video_command(in, reconfigure_round) ||
        !read_gst_video_command(in, add_round) ||
        !read_gst_video_command(in, stop_round) ||
        !read_gst_video_command(in, status_round) ||
        !read_gst_video_event(in, ready_round) ||
        !read_gst_video_event(in, failed_round)) {
        std::cerr << "gst worker codec self-test: read failed\n";
        return 1;
    }

    GstVideoGraph decoded;
    if (!gst_video_decode_graph(gst_video_encode_graph(graph), decoded) ||
        gst_video_encode_graph(decoded) != gst_video_encode_graph(graph)) {
        std::cerr << "gst worker codec self-test: graph blob mismatch\n";
        return 1;
    }

    const bool ok =
        commands_equal(start, start_round) &&
        commands_equal(graph_start, graph_round) &&
        commands_equal(reconfigure, reconfigure_round) &&
        commands_equal(add, add_round) &&
        commands_equal(stop, stop_round) &&
        commands_equal(status, status_round) &&
        ready_round.kind == GstVideoEventKind::Ready &&
        ready_round.message == ready.message &&
        failed_round.kind == GstVideoEventKind::Failed &&
        failed_round.message == failed.message;
    if (!ok) {
        std::cerr << "gst worker codec self-test: round-trip mismatch\n";
        return 1;
    }
    std::cout << "gst worker codec self-test: ok\n";
    return 0;
}

int run_ipc_test(const char* executable) {
    using archstreamer::GstVideoEventKind;
    using archstreamer::GstVideoWorkerClient;
    using archstreamer::gst_video_event_pipeline_id;
    using archstreamer::gst_video_pipeline_record_path;
    using archstreamer::gst_video_start_command;
    using archstreamer::gst_video_status_command;
    using archstreamer::gst_video_stop_command;

    GstVideoWorkerClient client;
    try {
        client.start(executable);
    } catch (const std::exception& error) {
        std::cerr << "gst worker ipc-test: " << error.what() << '\n';
        return 1;
    }

    const auto started = client.execute(gst_video_start_command(
        "videotestsrc is-live=true ! fakesink sync=false",
        "ipc-test"));
    if (started.kind != GstVideoEventKind::Ready) {
        std::cerr << "gst worker ipc-test: start failed: " << started.message << '\n';
        client.stop();
        return 1;
    }
    const auto id = gst_video_event_pipeline_id(started.message);
    if (id.empty()) {
        std::cerr << "gst worker ipc-test: ready event has no pipeline id\n";
        client.stop();
        return 1;
    }

    const auto status = client.execute(gst_video_status_command());
    if (status.kind != GstVideoEventKind::Status ||
        gst_video_event_pipeline_id(status.message) != id ||
        status.message.find("pipeline=" + id) == std::string::npos) {
        std::cerr << "gst worker ipc-test: status missing pipeline id: " << status.message << '\n';
        client.stop();
        return 1;
    }

    const auto record_path = gst_video_pipeline_record_path(id);
    std::ifstream record(record_path);
    const std::string body{
        std::istreambuf_iterator<char>(record),
        std::istreambuf_iterator<char>()};
    if (!record ||
        body.find("id=" + id + "\n") == std::string::npos ||
        body.find("pid=") == std::string::npos ||
        body.find("state=playing\n") == std::string::npos) {
        std::cerr << "gst worker ipc-test: record missing at " << record_path << '\n';
        client.stop();
        return 1;
    }

    const auto stopped = client.execute(gst_video_stop_command());
    if (stopped.kind != GstVideoEventKind::Stopped) {
        std::cerr << "gst worker ipc-test: stop failed: " << stopped.message << '\n';
        client.stop();
        return 1;
    }
    if (std::filesystem::exists(record_path)) {
        std::cerr << "gst worker ipc-test: record still present after stop\n";
        client.stop();
        return 1;
    }

    client.stop();
    std::cout << "gst worker ipc-test: ok id=" << id << '\n';
    return 0;
}

int run_worker() {
    std::cout << std::unitbuf;
    GstVideoEngine engine;
    std::mutex out_mutex;
    auto emit = [&](const GstVideoEvent& event) {
        std::lock_guard lock(out_mutex);
        if (!write_gst_video_event(std::cout, event)) {
            std::cerr << "gst worker: failed to write "
                      << gst_video_event_kind_name(event.kind) << " event\n";
        }
    };
    engine.set_event_handler(emit);

    while (true) {
        GstVideoCommand command;
        if (!read_gst_video_command(std::cin, command)) {
            break;
        }
        emit(engine.execute(command));
    }

    if (engine.running()) {
        engine.execute(gst_video_stop_command());
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::string_view{argv[1]} == "--codec-test") {
        return run_codec_self_test();
    }
    if (argc > 1 && std::string_view{argv[1]} == "--ipc-test") {
        return run_ipc_test(argv[0]);
    }
    if (argc > 1 && (std::string_view{argv[1]} == "--list-commands" ||
                     std::string_view{argv[1]} == "--help")) {
        gst_video_print_command_help(std::cout);
        return 0;
    }
    if (argc > 1) {
        std::cerr << "usage: archstreamer_gst_worker [--codec-test | --ipc-test | --list-commands]\n";
        return 2;
    }
    return run_worker();
}
