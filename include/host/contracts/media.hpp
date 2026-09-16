#pragma once

#include "host/contracts/audio.hpp"
#include "host/contracts/present.hpp"

#include <string>

namespace archstreamer {

// Preferred graphics API for standalone emulators (Yuzu). Ignored for RetroArch cores.
enum class GraphicsApiPreference {
    Auto,
    OpenGL,
    Vulkan,
};

/**
 * Combined capture request session passes into the media server.
 *
 * Video and audio still share one config object today (not yet cohesive).
 * Use present() / audio_tap() at concern boundaries instead of reading flags.
 */
struct GStreamerMediaCaptureConfig {
    bool video = false;
    bool audio = false;
    std::string virtual_display = ":99";
    std::string video_resolution = "1920x1080";
    VirtualDisplayBackend display_backend = VirtualDisplayBackend::None;
    AudioCaptureBackend audio_backend = AudioCaptureBackend::Pulse;
    std::string audio_source;
    bool verbose = false;
    // nvidia-smi index for the gst-launch nvenc process (CUDA_VISIBLE_DEVICES); -1 = default.
    int nvenc_cuda_device_id = -1;

    [[nodiscard]] PresentTarget present() const {
        PresentTarget target;
        target.backend = display_backend;
        target.display = virtual_display;
        return target;
    }

    [[nodiscard]] AudioTap audio_tap() const {
        return AudioTap{audio_backend, audio_source};
    }
};

} // namespace archstreamer
