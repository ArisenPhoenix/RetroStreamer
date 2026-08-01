#pragma once

#include "client/media_receiver.hpp"
#include "common/media.hpp"
#include "common/platform/default_platform.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace archstreamer {

class GStreamerMediaReceiver final : public MediaReceiver {
public:
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
     * Point video at a new RTP port, leaving audio on its live timeline.
     * Only call once the host confirms it is publishing there.
     */
    bool switch_video(const std::string& video_uri);

private:
    void start_audio_pipeline(bool wait_for_ready);
    void start_video_process(
        ChildProcess& process,
        std::uint16_t port,
        const std::filesystem::path& log_path,
        bool wait_for_ready);

    ChildProcess video_process_;
    ChildProcess audio_process_;
    MediaEndpoint endpoint_;
    std::string bound_audio_device_;
    std::string pending_audio_device_;
    int pending_audio_device_streak_ = 0;
    std::uint64_t bound_audio_epoch_ = 0;
    std::chrono::steady_clock::time_point next_audio_device_check_{};
    std::string video_pipeline_info_;
    std::string audio_pipeline_info_;
};

} // namespace archstreamer
