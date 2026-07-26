#include "host/launch_environment.hpp"

#include "common/platform/process_utils.hpp"
#ifndef _WIN32
#include "host/virtual_display.hpp"

#include <filesystem>
#include <unistd.h>
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

void ProcessEnvironment::add_unset(std::string key) {
    for (const auto& existing : unset) {
        if (existing == key) {
            return;
        }
    }
    unset.push_back(std::move(key));
}

void ProcessEnvironment::merge(const ProcessEnvironment& other) {
    for (const auto& key : other.unset) {
        add_unset(key);
    }
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

#ifndef _WIN32
namespace {

// Private XDG_RUNTIME_DIR without Wayland sockets. Pulse uses PULSE_SERVER —
// do not symlink pulse/ here (libpulse rejects that).
std::string prepare_x11_capture_runtime_dir() {
    const auto dir =
        std::filesystem::path{"/tmp"} / ("archstreamer-xdg-" + std::to_string(getpid()));
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        return {};
    }
    std::filesystem::permissions(
        dir,
        std::filesystem::perms::owner_all,
        std::filesystem::perm_options::replace,
        ec);
    return dir.string();
}

std::filesystem::path real_xdg_runtime_dir() {
    if (const char* env_rt = std::getenv("XDG_RUNTIME_DIR");
        env_rt != nullptr && env_rt[0] != '\0') {
        return env_rt;
    }
    return std::filesystem::path{"/run/user"} / std::to_string(geteuid());
}

} // namespace

void cleanup_x11_capture_runtime_dir() {
    const auto dir =
        std::filesystem::path{"/tmp"} / ("archstreamer-xdg-" + std::to_string(getpid()));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}
#endif

#ifdef _WIN32
void cleanup_x11_capture_runtime_dir() {}
#endif

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
        // Prefer a small Pulse quantum so audio_sync pacing stays near realtime.
        env.set("PULSE_LATENCY_MSEC", "40");
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
    } else {
        // Xvfb/Xephyr/VirtualGL: pin the emulator to the capture DISPLAY and strip
        // Wayland so RetroArch/SDL/Qt cannot attach to the host compositor (visible
        // game on the host desktop + black ximagesrc for clients).
        if (!capture_display.empty()) {
            env.set("DISPLAY", capture_display);
        }
        env.add_unset("WAYLAND_DISPLAY");
        env.add_unset("WAYLAND_SOCKET");
        env.set("XDG_SESSION_TYPE", "x11");
        env.set("GDK_BACKEND", "x11");
        env.set("SDL_VIDEODRIVER", "x11");
        env.set("QT_QPA_PLATFORM", "xcb");
        if (const auto runtime = prepare_x11_capture_runtime_dir(); !runtime.empty()) {
            env.set("XDG_RUNTIME_DIR", runtime);
            const auto real_rt = real_xdg_runtime_dir();
            // Keep Pulse/PipeWire on the real session while XDG_RUNTIME_DIR stays
            // Wayland-free (private dir has no wayland-0 socket).
            env.set("PIPEWIRE_RUNTIME_DIR", real_rt.string());
            const auto pulse_native = real_rt / "pulse" / "native";
            std::error_code ec;
            if (std::filesystem::exists(pulse_native, ec) && !ec) {
                env.set("PULSE_SERVER", "unix:" + pulse_native.string());
            }
        }
        if (virtualgl_capture) {
            env.merge_pairs(virtual_gl_environment());
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
