#pragma once

#include "client/media_receiver.hpp"
#include "client/video_embed_bridge.hpp"
#include "client/video_window_geometry.hpp"
#include "common/media.hpp"
#include "common/platform/default_platform.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace archstreamer {

class GStreamerOverlayVideo;

class GStreamerMediaReceiver final : public MediaReceiver {
public:
    GStreamerMediaReceiver();
    ~GStreamerMediaReceiver() override;

    void connect(const MediaEndpoint& endpoint) override;
    void disconnect() override;
    bool poll() override;
    bool video_running() const;
    bool audio_running() const;
    bool video_frames_seen() const;
    // Best-effort count of decoded frames from gst progressreport log lines.
    std::uint64_t decoded_frame_count() const;
    const std::string& video_pipeline_info() const;
    const std::string& audio_pipeline_info() const;

    /** Restart Opus only — used when video lagged and audio free-ran ahead. */
    bool restart_audio();

    /**
     * When set before connect/switch, video paints into this X11 window (Qt surface)
     * via in-process GstVideoOverlay. 0 = standalone gst-launch window.
     */
    void set_embed_xid(std::uint64_t xid);
    std::uint64_t embed_xid() const { return embed_xid_; }

    /** Shared with the Qt surface for appsink frames + emergency stop on close. */
    void set_video_embed_bridge(std::shared_ptr<VideoEmbedBridge> bridge);

    /**
     * Warm a second decode+sink on the host staging RTP port (standalone only).
     * With embed_xid, uses a headless probe then cold-restarts into the same window.
     */
    bool begin_pending_video(const std::string& video_uri);
    std::optional<std::string> poll_pending_ready();
    bool pending_video_active() const;

    void abort_pending_video();

    bool promote_or_switch_video(const std::string& video_uri);
    bool switch_video(const std::string& video_uri);

    /** Push Qt widget size into GstVideoOverlay (letterbox within the surface). */
    void apply_video_overlay_geometry(int width, int height);
    void expose_video_overlay();

private:
    void start_audio_pipeline(bool wait_for_ready);
    void start_video_process(
        ChildProcess& process,
        std::uint16_t port,
        const std::filesystem::path& log_path,
        bool wait_for_ready,
        bool update_pipeline_info);
    void start_overlay_video(std::uint16_t port);
    bool primary_video_running() const;
    void try_park_pending_offscreen();
    bool reveal_pending_on_target(std::chrono::milliseconds timeout);
    void refresh_pending_target_from_primary();
    bool begin_embed_staging_probe(const std::string& video_uri);
    std::optional<std::string> poll_embed_staging_probe();

    ChildProcess video_process_;
    ChildProcess audio_process_;
    ChildProcess pending_video_;
    std::unique_ptr<GStreamerOverlayVideo> overlay_video_;
    std::shared_ptr<VideoEmbedBridge> video_embed_bridge_;
    MediaEndpoint endpoint_;
    std::uint64_t embed_xid_ = 0;
    int overlay_width_ = 0;
    int overlay_height_ = 0;
    std::string pending_video_uri_;
    std::string pending_pipeline_info_;
    VideoWindowGeometry pending_target_geometry_{};
    std::chrono::steady_clock::time_point pending_started_{};
    bool pending_active_ = false;
    bool pending_ack_sent_ = false;
    bool pending_offscreen_parked_ = false;
    bool embed_probe_mode_ = false;
    std::string bound_audio_device_;
    std::string pending_audio_device_;
    int pending_audio_device_streak_ = 0;
    std::uint64_t bound_audio_epoch_ = 0;
    std::chrono::steady_clock::time_point next_audio_device_check_{};
    std::string video_pipeline_info_;
    std::string audio_pipeline_info_;
};

} // namespace archstreamer
