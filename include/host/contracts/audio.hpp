#pragma once

#include <string>

namespace archstreamer {

enum class AudioCaptureBackend {
    Pulse,
    PipeWire,
    // Windows host: WASAPI loopback via wasapisrc.
    Wasapi,
};

/**
 * Shared audio-tap language.
 *
 * Audio owns the null-sink / loopback. Emulators get `source` as PULSE_SINK
 * (or equivalent). Video/audio fanout captures `source` (often a `.monitor`).
 */
struct AudioTap {
    AudioCaptureBackend backend = AudioCaptureBackend::Pulse;
    std::string source;

    [[nodiscard]] bool captures() const { return !source.empty(); }
};

} // namespace archstreamer
