#pragma once

#include "common/media.hpp"
#include "common/platform/default_platform.hpp"
#include "common/protocol.hpp"
#include "host/contracts/audio.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace archstreamer {

// One pulsesrc/opus encode shared by Watch-local + all remotes (multiudpsink).
// Composed by GStreamerMediaServer; not a second session-facing media object.
class GStreamerAudioFanout {
public:
    ~GStreamerAudioFanout();

    std::vector<MediaClientStream> start(
        AudioCaptureBackend backend,
        const std::string& source,
        const std::vector<MediaStreamRequest>& destinations);
    MediaClientStream add(
        AudioCaptureBackend backend,
        const std::string& source,
        const MediaStreamRequest& destination);
    void stop();
    void stop_client(ClientId client_id);
    /** Rebuild the shared Opus encode (pair with video ladder restarts for A/V realign). */
    void restart();

private:
    struct Destination {
        ClientId client_id = 0;
        std::string host;
        std::uint16_t port = 0;
    };

    void restart_pipeline();

    AudioCaptureBackend backend_ = AudioCaptureBackend::Pulse;
    std::string source_;
    std::vector<Destination> destinations_;
    ChildProcess process_;
};

} // namespace archstreamer
