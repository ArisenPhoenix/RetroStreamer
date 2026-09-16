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

inline GStreamerMediaCaptureConfig make_media_capture_config(
    bool video,
    bool audio,
    const PresentTarget& present,
    const AudioTap& audio_tap,
    std::string video_resolution,
    bool verbose,
    int nvenc_cuda_device_id) {
    GStreamerMediaCaptureConfig config;
    config.video = video;
    config.audio = audio;
    config.virtual_display = present.display;
    config.video_resolution = std::move(video_resolution);
    config.display_backend = present.backend;
    config.audio_backend = audio_tap.backend;
    config.audio_source = audio_tap.source;
    config.verbose = verbose;
    config.nvenc_cuda_device_id = nvenc_cuda_device_id;
    return config;
}

} // namespace archstreamer
