#include "host/launch_environment.hpp"

#include "common/platform/process_utils.hpp"
#ifndef _WIN32
#include "host/virtual_display.hpp"
#endif

namespace archstreamer {
namespace {

void upsert(
    std::vector<std::pair<std::string, std::string>>& entries,
    std::string key,
    std::string value) {
    for (auto& entry : entries) {
        if (entry.first == key) {
            entry.second = std::move(value);
            return;
        }
    }
    entries.emplace_back(std::move(key), std::move(value));
}

} // namespace

void ProcessEnvironment::set(std::string key, std::string value) {
    upsert(entries, std::move(key), std::move(value));
}

void ProcessEnvironment::merge(const ProcessEnvironment& other) {
    for (const auto& [key, value] : other.entries) {
        upsert(entries, key, value);
    }
}

void ProcessEnvironment::merge_pairs(
    const std::vector<std::pair<std::string, std::string>>& pairs) {
    for (const auto& [key, value] : pairs) {
        upsert(entries, key, value);
    }
}

ProcessEnvironment audio_launch_environment(
    bool stream_media,
    bool stream_audio,
    bool host_plays_locally,
    const std::string& audio_source) {
    ProcessEnvironment env;
    if (stream_media) {
        env.set("SDL_AUDIODRIVER", "pulse");
    }
    if (stream_audio) {
        if (!audio_source.empty() && audio_source.ends_with(".monitor")) {
            env.set("PULSE_SINK", audio_source.substr(0, audio_source.size() - 8));
        }
    } else if (host_plays_locally) {
        // Keep Host Player on the real default sink even if a prior stream left
        // PULSE_SINK=archstreamer in the GUI process environment.
        env.set("SDL_AUDIODRIVER", "pulse");
        const auto default_sink = read_command_output("pactl get-default-sink 2>/dev/null");
        if (!default_sink.empty() && default_sink != "archstreamer") {
            env.set("PULSE_SINK", default_sink);
        }
    }
    return env;
}

ProcessEnvironment input_launch_environment(const std::string& ignore_devices) {
    ProcessEnvironment env;
    if (!ignore_devices.empty()) {
        env.set("SDL_GAMECONTROLLER_IGNORE_DEVICES", ignore_devices);
    }
    return env;
}

ProcessEnvironment capture_launch_environment(
    bool use_virtual_capture,
    bool gamescope_capture,
    bool virtualgl_capture,
    const std::string& capture_display) {
    ProcessEnvironment env;
    if (!use_virtual_capture) {
        return env;
    }
#ifndef _WIN32
    // Mutually exclusive capture backends.
    if (gamescope_capture) {
        env.merge_pairs(gamescope_launch_environment());
    } else if (virtualgl_capture) {
        // Xvfb/Xephyr/VirtualGL need DISPLAY=:99. Gamescope owns nested Xwayland itself.
        if (!capture_display.empty()) {
            env.set("DISPLAY", capture_display);
        }
        env.merge_pairs(virtual_gl_environment());
    } else {
        if (!capture_display.empty()) {
            env.set("DISPLAY", capture_display);
        }
    }
#else
    (void)gamescope_capture;
    (void)virtualgl_capture;
    (void)capture_display;
#endif
    return env;
}

ProcessEnvironment build_emulator_launch_environment(const EmulatorLaunchEnvRequest& request) {
    ProcessEnvironment env;
    env.merge(audio_launch_environment(
        request.stream_media,
        request.stream_audio,
        request.host_plays_locally,
        request.audio_source));
    env.merge(input_launch_environment(request.ignore_devices));
    if (request.render_gpu.has_value()) {
        env.merge_pairs(render_gpu_environment(*request.render_gpu));
    }
    env.merge(capture_launch_environment(
        request.use_virtual_capture,
        request.gamescope_capture,
        request.virtualgl_capture,
        request.capture_display));
    if (request.yuzu_profile.has_value()) {
        env.merge_pairs(yuzu_launch_environment(*request.yuzu_profile));
    }
    return env;
}

} // namespace archstreamer
