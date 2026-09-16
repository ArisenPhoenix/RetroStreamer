#pragma once

#include "common/media.hpp"
#include "host/contracts/gpu.hpp"
#include "host/contracts/present.hpp"
#include "host/video/video_settings.hpp"

#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace archstreamer {

class MediaServer;

/**
 * Runtime capture surface bound for a session. Video owns this; session
 * fills it from PresentTarget + GPU selection + VideoSettings.resolution.
 */
struct SessionCaptureDevices {
    std::optional<GpuTarget> resolved_gpu;
    PresentTarget present;
    std::string video_resolution;

    void bind(
        const PresentTarget& bound_present,
        std::optional<GpuTarget> gpu,
        std::string resolution) {
        present = bound_present;
        resolved_gpu = std::move(gpu);
        video_resolution = std::move(resolution);
    }

    std::string info() const {
        std::ostringstream o;
        o << "Capture: " << (present.captures() ? "fullscreen" : "windowed")
          << video_resolution
          << " on display " << present.display
          << (present.captures() && !present.uses_desktop()
                  ? " (virtual)"
                  : " (host)")
          << '\n';
        return o.str();
    }
};

/**
 * Facts video needs to pick a present surface. Session fills emulator-only
 * bits; host video knobs come from VideoSettings.
 */
struct CaptureResolveInput {
    bool stream_video = false;
    bool standalone = false;
    bool core_needs_gl = false;
    VirtualDisplayBackend preferred_backend = VirtualDisplayBackend::None;
    std::string capture_display;
    /** May be capped for software cores (caller copies back to VideoSettings). */
    std::string video_resolution;
};

inline CaptureResolveInput make_capture_resolve_input(
    const VideoSettings& video,
    bool standalone,
    bool core_needs_gl) {
    CaptureResolveInput input;
    input.stream_video = video.enabled;
    input.standalone = standalone;
    input.core_needs_gl = core_needs_gl;
    input.preferred_backend = video.preferred_backend;
    input.capture_display = video.virtual_display;
    input.video_resolution = video.resolution;
    return input;
}

/**
 * Choose display backend and optionally cap resolution for software cores.
 * Platform-specific: Linux may pick Gamescope/VirtualGL/Xvfb; Windows Desktop.
 */
PresentTarget resolve_present_target(CaptureResolveInput& input);

/** Resolve PresentTarget and write any software-core cap back into VideoSettings. */
inline PresentTarget resolve_present_target_from_settings(
    VideoSettings& video,
    bool standalone,
    bool core_needs_gl) {
    auto input = make_capture_resolve_input(video, standalone, core_needs_gl);
    auto present = resolve_present_target(input);
    video.resolution = std::move(input.video_resolution);
    return present;
}

/**
 * Apply gamescope / VirtualGL / desktop-capture command prefixes for standalone (Yuzu).
 * Logs the chosen path. Throws if a required Linux tool is missing.
 */
void apply_standalone_capture_prefix(
    std::vector<std::string>& command_prefix,
    const PresentTarget& present,
    const std::string& video_resolution,
    const std::string& gamescope_vk_device);

/**
 * Wrap RetroArch with VirtualGL when present uses VirtualGL (Linux only).
 */
void apply_retroarch_vgl_prefix(
    std::vector<std::string>& command_prefix,
    const PresentTarget& present,
    const std::optional<GpuTarget>& resolved_gpu);

/**
 * If media deferred PipeWire video (gamescope), wait for the node and start it.
 * No-op on Windows / when not deferred.
 * owner_pid: gamescope wrapper pid when known; used to avoid latching a stale node.
 */
void start_deferred_gamescope_video_if_needed(
    MediaServer* media_server,
    const std::string& video_resolution,
    std::vector<MediaClientStream>& media_streams,
    int owner_pid = 0);

/** True when this platform supports Gamescope capture paths. */
bool platform_supports_gamescope_capture();

/** True when this platform supports VirtualGL capture paths. */
bool platform_supports_virtualgl_capture();

} // namespace archstreamer
