#pragma once

#include "common/protocol.hpp"

#include <cstdint>
#include <string>

namespace archstreamer {

enum class VirtualDisplayBackend {
    None,
    Xvfb,
    Xephyr,
    // Xvfb for capture + VirtualGL (vglrun) so OpenGL uses the host GPU.
    // Kept for revisit; Switch streaming prefers Gamescope.
    VirtualGL,
    // Headless gamescope compositor + PipeWire video source (Vulkan-capable).
    Gamescope,
};

enum class AudioCaptureBackend {
    Pulse,
    PipeWire,
    // Windows host: WASAPI loopback via wasapisrc.
    Wasapi,
};

// Preferred graphics API for standalone emulators (Yuzu). Ignored for RetroArch cores.
enum class GraphicsApiPreference {
    Auto,
    OpenGL,
    Vulkan,
};

// Capture config passed into make_host_media_server (Linux GStreamer or Windows DXGI/WASAPI).
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
};

} // namespace archstreamer
