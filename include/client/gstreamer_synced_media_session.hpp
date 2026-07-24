#pragma once

#include "client/media_receiver.hpp"
#include "common/media.hpp"
#include "common/platform/default_platform.hpp"

#include <chrono>
#include <cstdint>
#include <string>

namespace archstreamer {

// Experimental A/V path: one gst-launch process so video and audio sinks share a
// pipeline clock (sync=true). Keep GStreamerMediaReceiver for the working dual-process path.
class GStreamerSyncedMediaSession {
public:
    class VideoBranch {
    public:
        bool enabled() const { return enabled_; }
        bool running() const;
        const std::string& pipeline_info() const { return pipeline_info_; }

    private:
        friend class GStreamerSyncedMediaSession;
        GStreamerSyncedMediaSession* session_ = nullptr;
        bool enabled_ = false;
        std::string pipeline_info_;
    };

    class AudioBranch {
    public:
        bool enabled() const { return enabled_; }
        bool running() const;
        const std::string& pipeline_info() const { return pipeline_info_; }

    private:
        friend class GStreamerSyncedMediaSession;
        GStreamerSyncedMediaSession* session_ = nullptr;
        bool enabled_ = false;
        std::string pipeline_info_;
    };

    GStreamerSyncedMediaSession();
    ~GStreamerSyncedMediaSession();

    GStreamerSyncedMediaSession(const GStreamerSyncedMediaSession&) = delete;
    GStreamerSyncedMediaSession& operator=(const GStreamerSyncedMediaSession&) = delete;

    void connect(const MediaEndpoint& endpoint);
    void disconnect();
    bool running() const;

    VideoBranch& video() { return video_; }
    const VideoBranch& video() const { return video_; }
    AudioBranch& audio() { return audio_; }
    const AudioBranch& audio() const { return audio_; }

    bool video_frames_seen() const;
    std::uint64_t decoded_frame_count() const;

private:
    ChildProcess process_;
    VideoBranch video_;
    AudioBranch audio_;
    std::string stderr_log_path_;
};

// Drop-in MediaReceiver that owns a GStreamerSyncedMediaSession.
class GStreamerSyncedMediaReceiver final : public MediaReceiver {
public:
    void connect(const MediaEndpoint& endpoint) override;
    void disconnect() override;
    bool poll() override;

    bool video_running() const;
    bool audio_running() const;
    bool video_frames_seen() const;
    std::uint64_t decoded_frame_count() const;
    const std::string& video_pipeline_info() const;
    const std::string& audio_pipeline_info() const;

    GStreamerSyncedMediaSession& session() { return session_; }
    const GStreamerSyncedMediaSession& session() const { return session_; }

private:
    GStreamerSyncedMediaSession session_;
    MediaEndpoint endpoint_;
    std::string bound_audio_device_;
    std::chrono::steady_clock::time_point next_audio_device_check_{};
};

} // namespace archstreamer
