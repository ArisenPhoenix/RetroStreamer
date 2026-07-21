#pragma once

#include "client/media_receiver.hpp"
#include "common/platform/default_platform.hpp"

namespace archstreamer {

class GStreamerMediaReceiver final : public MediaReceiver {
public:
    void connect(const MediaEndpoint& endpoint) override;
    void disconnect() override;

private:
    ChildProcess video_process_;
    ChildProcess audio_process_;
};

} // namespace archstreamer
