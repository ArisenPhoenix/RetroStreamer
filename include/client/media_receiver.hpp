#pragma once

#include "common/media.hpp"

namespace archstreamer {

class MediaReceiver {
public:
    virtual ~MediaReceiver() = default;

    virtual void connect(const MediaEndpoint& endpoint) = 0;
    virtual void disconnect() = 0;
    // Optional: rebind playback when the default audio device changes (hot-plug).
    // Returns true when the audio path was restarted.
    virtual bool poll() { return false; }
};

} // namespace archstreamer
