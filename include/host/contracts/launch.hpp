#pragma once

#include "host/contracts/audio.hpp"
#include "host/contracts/present.hpp"
#include "host/contracts/seat.hpp"

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

/**
 * One emulator process plug: present surface + audio tap + seat.
 *
 * Session fills this from video/audio/peripherals; build_emulator_launch_environment
 * applies it.
 */
struct EmulatorPlug {
    PresentTarget present;
    AudioTap audio;
    std::optional<PadPlan> seat;
};

// Resolved decisions from host_app; the assembler owns merge order so capture
// backends cannot be applied without their matching env layer.
// GPU and emulator-profile layers are ProcessEnvironment bags filled by
// hardware/console — contracts do not include those types.
struct EmulatorLaunchEnvRequest {
    bool stream_media = false;       // config.media.enabled()
    bool stream_audio = false;       // config.media.audio.enabled
    bool host_plays_locally = false; // Host Player without remote stream
    EmulatorPlug plug;
    /** Fallback when plug.seat is unset (prefer PadPlan for all new paths). */
    std::string ignore_devices;
    /** Lobby / SessionManager id; injected as ARCHSTREAMER_SESSION_ID. */
    std::string session_id;

    /** PRIME / Vulkan device env from hardware (`render_gpu_environment` on GpuTarget). */
    ProcessEnvironment gpu_env;
    /** XDG / SDL profile env from the console backend. */
    ProcessEnvironment profile_env;
};

// Layer builders (also usable for tests / diagnostics).
ProcessEnvironment audio_launch_environment(
    bool stream_media,
    bool stream_audio,
    bool host_plays_locally,
    const AudioTap& audio);

ProcessEnvironment input_launch_environment(const std::string& ignore_devices);

ProcessEnvironment capture_launch_environment(const PresentTarget& present);

// Composition order: audio → input → gpu → capture → emulator profile.
ProcessEnvironment build_emulator_launch_environment(const EmulatorLaunchEnvRequest& request);

// Remove /tmp/archstreamer-xdg-<pid> left by capture_launch_environment (no-op on Windows).
void cleanup_x11_capture_runtime_dir();

} // namespace archstreamer
