#pragma once

#include "host/host_app_config.hpp"
#include "host/media_capture.hpp"
#include "host/media_server.hpp"
#include "host/retroarch_process.hpp"

#include <optional>
#include <string>
#include <vector>

namespace archstreamer {

struct GpuDevice;

/** Resolved capture topology for one emulator launch (platform-neutral). */
struct CapturePlan {
    bool use_virtual_capture = false;
    bool capture_fullscreen = false;
    bool gamescope_capture = false;
    bool virtualgl_capture = false;
    VirtualDisplayBackend display_backend = VirtualDisplayBackend::None;
    std::string capture_display;
};

/**
 * Choose display backend / capture flags and optionally cap resolution for software cores.
 * Platform-specific: Linux may pick Gamescope/VirtualGL/Xvfb; Windows leaves backend alone.
 */
CapturePlan resolve_capture_plan(
    HostAppConfig& config,
    const RetroArchLaunchConfig& launch_config);

/** Force Wasapi on Windows when Pulse was requested; no-op elsewhere. */
void normalize_audio_backend_for_platform(HostAppConfig& config);

/**
 * Apply gamescope / VirtualGL / desktop-capture command prefixes for standalone (Yuzu).
 * Logs the chosen path. Throws if a required Linux tool is missing.
 */
void apply_standalone_capture_prefix(
    RetroArchLaunchConfig& launch_config,
    const CapturePlan& capture,
    const HostAppConfig& config,
    const std::string& gamescope_vk_device);

/**
 * Wrap RetroArch with VirtualGL when capture.virtualgl_capture is set (Linux only).
 */
void apply_retroarch_vgl_prefix(
    RetroArchLaunchConfig& launch_config,
    const CapturePlan& capture,
    const std::optional<GpuDevice>& resolved_gpu);

/**
 * If media deferred PipeWire video (gamescope), wait for the node and start it.
 * No-op on Windows / when not deferred.
 */
void start_deferred_gamescope_video_if_needed(
    MediaServer* media_server,
    const HostAppConfig& config,
    std::vector<MediaClientStream>& media_streams);

/** True when this platform supports Gamescope capture paths. */
bool platform_supports_gamescope_capture();

/** True when this platform supports VirtualGL capture paths. */
bool platform_supports_virtualgl_capture();

} // namespace archstreamer
