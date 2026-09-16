#pragma once

#include <string>

namespace archstreamer {

/** How the host presents emulator frames for capture. Video owns the surface. */
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

/**
 * Shared present-surface language.
 *
 * Video produces this (Xvfb, gamescope/PipeWire, desktop). Emulators consume
 * `display` / launch env; session uses it to start capture. Fill pipewire_*
 * after the compositor publishes a node.
 */
struct PresentTarget {
    VirtualDisplayBackend backend = VirtualDisplayBackend::None;
    std::string display;
    std::string pipewire_path;
    std::string pipewire_target_object;
    std::string xtest_display;

    [[nodiscard]] bool captures() const {
        return backend != VirtualDisplayBackend::None;
    }
    [[nodiscard]] bool uses_pipewire() const {
        return backend == VirtualDisplayBackend::Gamescope;
    }
    [[nodiscard]] bool uses_x11_capture() const {
        return backend == VirtualDisplayBackend::Xvfb ||
            backend == VirtualDisplayBackend::Xephyr ||
            backend == VirtualDisplayBackend::VirtualGL;
    }
};

} // namespace archstreamer
