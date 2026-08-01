#include "host/launch_environment.hpp"

#include "common/platform/process_utils.hpp"
#include "host/streaming_audio_sink.hpp"
#include "host/virtual_display.hpp"

#include <filesystem>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace archstreamer {
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
            const auto sink = audio_source.substr(0, audio_source.size() - 8);
            env.set("PULSE_SINK", sink);
            // Tag this emulator so concurrent slots can park/capture independently.
            if (StreamingAudioSink::is_streaming_sink_name(sink)) {
                if (sink == StreamingAudioSink::kName) {
                    env.set("PULSE_PROP_application.id", StreamingAudioSink::kName);
                } else if (sink.rfind("archstreamer-", 0) == 0) {
                    try {
                        const int slot = std::stoi(sink.substr(std::string("archstreamer-").size()));
                        env.set(
                            "PULSE_PROP_application.id",
                            StreamingAudioSink::slot_application_id(slot));
                    } catch (const std::exception&) {
                        env.set("PULSE_PROP_application.id", sink);
                    }
                }
            }
        }
        // Prefer a small Pulse quantum so audio_sync pacing stays near realtime.
        env.set("PULSE_LATENCY_MSEC", "40");
    } else if (host_plays_locally) {
        // Keep Host Player on the real default sink even if a prior stream left
        // PULSE_SINK=archstreamer in the GUI process environment.
        env.set("SDL_AUDIODRIVER", "pulse");
        const auto default_sink = read_command_output("pactl get-default-sink 2>/dev/null");
        if (!default_sink.empty() && !StreamingAudioSink::is_streaming_sink_name(default_sink)) {
            env.set("PULSE_SINK", default_sink);
        }
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
    return env;
}

} // namespace archstreamer
