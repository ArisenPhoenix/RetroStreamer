#pragma once

#include "host/gpu_select.hpp"
#include "host/nds/melonds_user_profile.hpp"
#include "host/pad_plan.hpp"
#include "host/standalone_emulator.hpp"

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace archstreamer {

// Ordered key/value bag for child-process environments. set/merge replace by key
// so accidental duplicate pushes cannot leave stale values. unset is applied
// before set when spawning (needed to strip inherited Wayland for Xvfb capture).
struct ProcessEnvironment {
    std::vector<std::pair<std::string, std::string>> entries;
    std::vector<std::string> unset;

    void set(std::string key, std::string value);
    void add_unset(std::string key);
    /** Remove from entries and unset on spawn (avoids setenv putting a stale value back). */
    void clear_var(const std::string& key);
    void merge(const ProcessEnvironment& other);
    void merge_pairs(const std::vector<std::pair<std::string, std::string>>& pairs);
};

// Resolved decisions from host_app; the assembler owns merge order so capture
// backends cannot be applied without their matching env layer.
struct EmulatorLaunchEnvRequest {
    bool stream_media = false;       // config.audio || config.video
    bool stream_audio = false;       // config.audio
    bool host_plays_locally = false; // Host Player without remote stream
    std::string audio_source;        // may be "*.monitor" when capturing a sink
    /** Fallback when pad_plan is unset (prefer PadPlan for all new paths). */
    std::string ignore_devices;
    /**
     * Session pad contract (Shared IGNORE vs Exclusive EXCEPT). When set,
     * apply_pad_plan owns SDL filter env — profile launch envs must not set them.
     */
    std::optional<PadPlan> pad_plan;

    bool use_virtual_capture = false;
    bool gamescope_capture = false;
    bool virtualgl_capture = false;
    std::string capture_display; // DISPLAY=:99 for Xvfb/Xephyr/VGL

    std::optional<GpuDevice> render_gpu;
    std::optional<YuzuUserProfile> yuzu_profile; // set for standalone Yuzu
    std::optional<RyujinxUserProfile> ryujinx_profile; // set for standalone Ryujinx
    std::optional<MelonDsUserProfile> melonds_profile; // set for standalone melonDS
};

// Layer builders (also usable for tests / diagnostics).
ProcessEnvironment audio_launch_environment(
    bool stream_media,
    bool stream_audio,
    bool host_plays_locally,
    const std::string& audio_source);

ProcessEnvironment input_launch_environment(const std::string& ignore_devices);

ProcessEnvironment capture_launch_environment(
    bool use_virtual_capture,
    bool gamescope_capture,
    bool virtualgl_capture,
    const std::string& capture_display);

// Composition order: audio → input → gpu → capture → emulator profile.
ProcessEnvironment build_emulator_launch_environment(const EmulatorLaunchEnvRequest& request);

// Remove /tmp/archstreamer-xdg-<pid> left by capture_launch_environment (no-op on Windows).
void cleanup_x11_capture_runtime_dir();

} // namespace archstreamer
