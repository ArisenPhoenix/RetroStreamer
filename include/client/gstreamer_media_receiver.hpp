#pragma once

#include "client/media_receiver.hpp"
#include "common/media.hpp"
#include "common/platform/default_platform.hpp"

#include <chrono>
#include <cstdint>
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
    // Best-effort count of decoded frames from gst identity log lines.
    std::uint64_t decoded_frame_count() const;
    const std::string& video_pipeline_info() const;
    const std::string& audio_pipeline_info() const;

private:
    void start_audio_pipeline(bool wait_for_ready);

    ChildProcess video_process_;
    ChildProcess audio_process_;
    MediaEndpoint endpoint_;
    std::string bound_audio_device_;
    std::chrono::steady_clock::time_point next_audio_device_check_{};
    std::string video_pipeline_info_;
    std::string audio_pipeline_info_;
};

} // namespace archstreamer
