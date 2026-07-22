#pragma once

#include "client/media_receiver.hpp"
#include "common/platform/default_platform.hpp"

namespace archstreamer {

class GStreamerMediaReceiver final : public MediaReceiver {
public:
    void connect(const MediaEndpoint& endpoint) override;
    void disconnect() override;
    bool video_running() const;
    bool audio_running() const;
    bool video_frames_seen() const;
    const std::string& video_pipeline_info() const;

private:
    ChildProcess video_process_;
    ChildProcess audio_process_;
    std::string video_pipeline_info_;
};

} // namespace archstreamer
