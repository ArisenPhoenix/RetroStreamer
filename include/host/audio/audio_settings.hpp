#pragma once

#include "host/contracts/audio.hpp"

#include <cstdint>
#include <string>

namespace archstreamer {

/**
 * Host-facing audio knobs. Audio owns this; HostAppConfig embeds it so session
 * fills AudioTap from here instead of loose HostAppConfig fields.
 */
struct AudioSettings {
    bool enabled = true;
    std::uint16_t port = 6004;
    std::string source;
    AudioCaptureBackend backend = AudioCaptureBackend::Pulse;

    [[nodiscard]] AudioTap tap() const { return AudioTap{backend, source}; }
};

} // namespace archstreamer
