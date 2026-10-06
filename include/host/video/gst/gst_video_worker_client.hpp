#pragma once

#include "host/video/gst/gst_video_command.hpp"

#include <functional>
#include <memory>

namespace archstreamer {

/**
 * Parent side of archstreamer_gst_worker. One process owns one GstVideoEngine.
 * Ready and Status events start with `id=<gst pipeline name>`. The worker
 * writes that id to gst_video_pipeline_record_path while the pipeline is playing.
 */
class GstVideoWorkerClient {
public:
    using EventHandler = std::function<void(const GstVideoEvent&)>;

    GstVideoWorkerClient();
    ~GstVideoWorkerClient();

    GstVideoWorkerClient(const GstVideoWorkerClient&) = delete;
    GstVideoWorkerClient& operator=(const GstVideoWorkerClient&) = delete;

    void start(const std::string& executable);
    void set_event_handler(EventHandler handler);
    GstVideoEvent execute(const GstVideoCommand& command);
    void stop();

    [[nodiscard]] bool running() const;
    [[nodiscard]] int pid() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace archstreamer
