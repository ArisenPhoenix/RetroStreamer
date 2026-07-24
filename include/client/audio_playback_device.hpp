#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace archstreamer {

struct AudioOutputDevice {
    // Stable id: "auto", Pulse sink name, or WASAPI device id.
    std::string id;
    std::string name;
    bool is_default = false;
};

std::vector<AudioOutputDevice> list_audio_output_devices();

// Preferred playback target for client / Watch-local receivers. "auto" follows the OS default.
void set_preferred_audio_output_device(std::string id);
std::string preferred_audio_output_device();
// Bumps when the preference changes so live receivers can rebind immediately.
std::uint64_t audio_output_preference_epoch();

// Effective device key currently selected (resolves "auto").
std::string current_audio_playback_device_key();

struct AudioPlaybackSink {
    std::vector<std::string> gst_args;
    std::string description = "autoaudiosink";
    std::string device_key;
};

// sync=true for the shared-clock A/V path; false for the legacy dual-process path.
AudioPlaybackSink choose_audio_playback_sink(bool sync);

} // namespace archstreamer
