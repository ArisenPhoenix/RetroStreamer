#pragma once

#include "client/video_embed_bridge.hpp"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

namespace archstreamer {

/**
 * In-process RTP H.264 → decode → appsink → RGBA frames into VideoEmbedBridge.
 * Avoids ximagesink/xvimagesink PutImage into a Qt xid (compositor stalls on close).
 */
class GStreamerOverlayVideo {
public:
    GStreamerOverlayVideo();
    ~GStreamerOverlayVideo();

    GStreamerOverlayVideo(const GStreamerOverlayVideo&) = delete;
    GStreamerOverlayVideo& operator=(const GStreamerOverlayVideo&) = delete;

    void set_frame_bridge(std::shared_ptr<VideoEmbedBridge> bridge);

    bool start(std::uint16_t udp_port, std::uint64_t window_handle = 0);
    /** Full stop: wait for NULL, free pipeline (session thread). */
    void stop();
    /**
     * Ask the pipeline to go NULL without waiting (safe on the GUI thread).
     * Session disconnect still calls stop() to join and free.
     */
    void request_stop();
    bool running() const;

    /** Cold restart on a new RTP port. */
    bool switch_port(std::uint16_t udp_port);

    void set_render_size(int width, int height);
    void expose();

    void publish_sample(const std::uint8_t* data, int width, int height, int stride);

    std::uint64_t frames_seen() const;
    const std::string& pipeline_info() const { return pipeline_info_; }

private:
    bool build_and_play(std::uint16_t udp_port);
    void teardown(bool wait_for_null);

    mutable std::mutex mutex_;
    void* pipeline_ = nullptr; // GstElement*
    void* bus_ = nullptr;      // GstBus*
    void* appsink_ = nullptr;  // GstElement*
    std::shared_ptr<VideoEmbedBridge> frame_bridge_;
    std::uint16_t port_ = 0;
    std::uint64_t frames_seen_ = 0;
    std::string pipeline_info_;
};

bool gstreamer_overlay_video_available();

} // namespace archstreamer
