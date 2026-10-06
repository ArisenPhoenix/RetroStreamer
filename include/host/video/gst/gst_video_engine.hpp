#pragma once

#include "host/video/gst/gst_video_command.hpp"

#include <functional>
#include <memory>

namespace archstreamer {

/**
 * Owns shared + staging GstElement graphs. No session or MediaServer types.
 *
 * execute() is synchronous (Ready/Failed/Stopped/Status). Runtime bus
 * Error/Warning/Eos go to the event handler. Graph mutators relaunch.
 */
class GstVideoEngine {
public:
    using EventHandler = std::function<void(const GstVideoEvent&)>;

    GstVideoEngine();
    ~GstVideoEngine();

    GstVideoEngine(const GstVideoEngine&) = delete;
    GstVideoEngine& operator=(const GstVideoEngine&) = delete;

    void set_event_handler(EventHandler handler);
    GstVideoEvent execute(const GstVideoCommand& command);
    [[nodiscard]] bool running() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace archstreamer
