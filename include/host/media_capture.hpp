#pragma once

#include "common/protocol.hpp"

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
};

// Preferred graphics API for standalone emulators (Yuzu). Ignored for RetroArch cores.
enum class GraphicsApiPreference {
    Auto,
    OpenGL,
    Vulkan,
};

} // namespace archstreamer
