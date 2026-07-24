#pragma once

#include <string>
#include <vector>

namespace archstreamer {

// Stable key for change detection (Pulse sink name, WASAPI device id, or empty).
std::string current_audio_playback_device_key();

struct AudioPlaybackSink {
    std::vector<std::string> gst_args;
    std::string description = "autoaudiosink";
    std::string device_key;
};

// sync=true for the shared-clock A/V path; false for the legacy dual-process path.
AudioPlaybackSink choose_audio_playback_sink(bool sync);

} // namespace archstreamer
