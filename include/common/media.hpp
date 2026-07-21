#pragma once

#include <cstdint>
#include <string>

namespace archstreamer {

struct MediaEndpoint {
    std::string video_uri;
    std::string audio_uri;
};

struct MediaStreamRequest {
    std::uint8_t client_id = 0;
    std::string destination_host;
    std::uint16_t port = 0;
};

struct MediaClientStream {
    std::uint8_t client_id = 0;
    std::string destination_host;
    MediaEndpoint endpoint;
};

} // namespace archstreamer
