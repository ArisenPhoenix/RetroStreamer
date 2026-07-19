#pragma once

#include "common/media.hpp"
#include "common/platform/default_platform.hpp"

#include <cstdint>
#include <string>

namespace archstreamer {

std::uint16_t video_port_from_endpoint(const MediaEndpoint& endpoint);
std::uint16_t audio_port_from_endpoint(const MediaEndpoint& endpoint);

class GStreamerMediaReceiver {
public:
    void connect(const MediaEndpoint& endpoint);
    void disconnect();

private:
    ChildProcess video_process_;
    ChildProcess audio_process_;
};

} // namespace archstreamer
