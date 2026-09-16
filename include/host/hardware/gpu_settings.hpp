#pragma once

#include <string>

namespace archstreamer {

/** Preferred graphics API for standalone Switch. Ignored for RetroArch. */
enum class GraphicsApiPreference {
    Auto,
    OpenGL,
    Vulkan,
};

/**
 * Host-facing GPU knobs. Hardware owns this; HostAppConfig embeds it so session
 * resolves encode/render cards from here instead of loose HostAppConfig fields.
 */
struct GpuSettings {
    // Primary GPU: H.264 nvenc encode, and game render unless separate_render_gpu.
    // "auto" or a GpuDevice::id from list_render_gpus() (e.g. nvidia:0).
    std::string encode_gpu = "auto";
    // When true, render_gpu selects the card for RetroArch/Switch GL/Vulkan (PRIME).
    bool separate_render_gpu = false;
    // Used when separate_render_gpu is true; otherwise encode_gpu is used for render too.
    std::string render_gpu = "auto";
    GraphicsApiPreference graphics_api = GraphicsApiPreference::Auto;

    [[nodiscard]] std::string effective_render_gpu() const {
        if (separate_render_gpu) {
            return render_gpu.empty() ? "auto" : render_gpu;
        }
        return encode_gpu.empty() ? "auto" : encode_gpu;
    }
};

} // namespace archstreamer
