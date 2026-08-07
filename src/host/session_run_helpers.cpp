#include "host/session_run_helpers.hpp"

#include "host/capture_platform.hpp"
#include "host/input_router.hpp"
#include "host/local_controller_bridge.hpp"
#include "host/platform/default_host_platform.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace archstreamer {
namespace {

std::mutex g_audio_park_mutex;

std::string with_prefix(std::string_view prefix, std::string_view message) {
    if (prefix.empty()) {
        return std::string(message);
    }
    return std::string(prefix) + std::string(message);
}

#ifndef _WIN32
std::string host_desktop_display() {
    if (const char* display = std::getenv("DISPLAY"); display != nullptr && *display != '\0') {
        return display;
    }
    return {};
}

/** ":0.0" and ":0" are the same screen for our purposes. */
std::string normalize_display_name(std::string name) {
    if (const auto dot = name.find('.'); dot != std::string::npos) {
        name.resize(dot);
    }
    return name;
}

bool is_host_desktop_display(const std::string& name) {
    const auto host = normalize_display_name(host_desktop_display());
    if (host.empty() || name.empty()) {
        return false;
    }
    return normalize_display_name(name) == host;
}

std::vector<std::string> gamescope_xtest_candidates(
    const std::string& preferred_display,
    std::optional<int> emulator_pid) {
    // When we know the session leader, reuse the soft-keyboard scoping rules:
    // only that process tree (nested Xwayland owned by it), never every local
    // X socket / :0–:10 probe. Sibling concurrent sessions must not share pause.
    if (emulator_pid.has_value() && *emulator_pid > 0) {
        return soft_keyboard_display_candidates(preferred_display, *emulator_pid);
    }

    std::vector<std::string> names;
    auto add = [&](const std::string& name) {
        if (name.empty()) {
            return;
        }
        // gamescope itself often inherits the host desktop DISPLAY=:0; XTest there
        // types into the user's real session (Space spam on the desktop). Never use it.
        if (is_host_desktop_display(name)) {
            return;
        }
        for (const auto& existing : names) {
            if (existing == name) {
                return;
            }
        }
        names.push_back(name);
    };

    add(preferred_display);

    struct SocketCandidate {
        std::string name;
        std::filesystem::file_time_type mtime{};
    };
    std::vector<SocketCandidate> sockets;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator("/tmp/.X11-unix", error)) {
        const auto filename = entry.path().filename().string();
        if (filename.size() < 2 || filename.front() != 'X') {
            continue;
        }
        const auto suffix = filename.substr(1);
        if (suffix.empty() || suffix.find_first_not_of("0123456789") != std::string::npos) {
            continue;
        }
        auto name = ":" + suffix;
        if (is_host_desktop_display(name)) {
            continue;
        }
        sockets.push_back({std::move(name), entry.last_write_time(error)});
    }
    std::sort(sockets.begin(), sockets.end(), [](const auto& a, const auto& b) {
        return a.mtime > b.mtime;
    });
    for (const auto& socket : sockets) {
        add(socket.name);
    }

    for (const auto& name : xtest_display_candidates(preferred_display)) {
        add(name);
    }
    return names;
}
#endif

} // namespace

bool plug_virtual_keyboard_with_retry(
    VirtualKeyboard& keyboard,
    bool use_virtual_capture,
    bool gamescope_capture,
    std::string_view log_prefix) {
    if (use_virtual_capture && !gamescope_capture) {
        for (int attempt = 0; attempt < 20; ++attempt) {
            try {
                keyboard.plug();
                return true;
            } catch (const std::exception& error) {
                if (attempt == 19) {
                    std::cerr
                        << with_prefix(log_prefix, "warning: virtual keyboard unavailable: ")
                        << error.what() << '\n';
                } else {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
            }
        }
        return false;
    }
    if (!use_virtual_capture) {
        try {
            keyboard.plug();
            return true;
        } catch (const std::exception& error) {
            std::cerr
                << with_prefix(log_prefix, "warning: virtual keyboard unavailable: ")
                << error.what() << '\n';
            return false;
        }
    }
    // gamescope: nested Xwayland is not up yet — plug after emulator start.
    return true;
}

bool plug_gamescope_virtual_keyboard_after_start(
    VirtualKeyboard& keyboard,
    const std::string& preferred_display,
    std::optional<int> emulator_pid,
    std::string_view log_prefix) {
#ifndef _WIN32
    if (keyboard.plugged()) {
        // Defensive: never leave a gamescope session bound to the host desktop
        // or a sibling session's nested Xwayland.
        if (is_host_desktop_display(keyboard.capture_display())
            || (emulator_pid.has_value()
                && !display_belongs_to_process_tree(
                        keyboard.capture_display(), *emulator_pid))) {
            keyboard.unplug();
        } else {
            if (emulator_pid.has_value()) {
                keyboard.set_target_pid(*emulator_pid);
            }
            return true;
        }
    }
    // Rebuild candidates each attempt — nested Xwayland may appear mid-loop.
    for (int attempt = 0; attempt < 50; ++attempt) {
        const auto candidates = gamescope_xtest_candidates(preferred_display, emulator_pid);
        for (const auto& name : candidates) {
            if (emulator_pid.has_value()
                && !display_belongs_to_process_tree(name, *emulator_pid)
                && name != preferred_display) {
                // Preferred is ARCHSTREAMER_XTEST_DISPLAY from launch — try it even
                // before the child environ/socket ownership checks can see it.
                continue;
            }
            try {
                keyboard.rebind_display(name);
                if (emulator_pid.has_value()) {
                    keyboard.set_target_pid(*emulator_pid);
                }
                keyboard.plug();
                if (is_host_desktop_display(keyboard.capture_display())) {
                    keyboard.unplug();
                    continue;
                }
                if (emulator_pid.has_value()
                    && !display_belongs_to_process_tree(
                            keyboard.capture_display(), *emulator_pid)
                    && keyboard.capture_display() != preferred_display) {
                    keyboard.unplug();
                    continue;
                }
                if (emulator_pid.has_value()) {
                    register_session_xtest_display_for_owner(
                        *emulator_pid, keyboard.capture_display());
                }
                return true;
            } catch (const std::exception&) {
                // Try next candidate / retry.
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    std::cerr
        << with_prefix(
               log_prefix,
               "warning: remoted keyboard (Space=FF) unavailable — "
               "could not open gamescope nested Xwayland with XTest "
               "(refusing host desktop DISPLAY)\n");
    return false;
#else
    (void)keyboard;
    (void)preferred_display;
    (void)emulator_pid;
    (void)log_prefix;
    return true;
#endif
}

std::unique_ptr<MediaServer> start_host_media_server_if_needed(
    const HostMediaStartRequest& req) {
    if (!req.config.audio && !req.config.video) {
        return nullptr;
    }
    normalize_audio_backend_for_platform(req.config);
    auto media_server = make_host_media_server(GStreamerMediaCaptureConfig{
        req.config.video,
        req.config.audio,
        req.capture_display,
        req.config.video_resolution,
        req.display_backend,
        req.config.audio_backend,
        req.config.audio_source,
        req.config.verbose,
        req.nvenc_cuda_device_id,
    });
    media_server->start(req.media_config, req.destinations, req.streams);
    return media_server;
}

void park_session_game_audio(
    StreamingAudioSink* audio,
    std::optional<int> audio_slot_index) {
    if (audio == nullptr) {
        return;
    }
    if (audio_slot_index.has_value()) {
        std::lock_guard lock(g_audio_park_mutex);
        audio->park_game_audio_for_slot(*audio_slot_index);
        return;
    }
    audio->park_game_audio();
}

void park_session_game_audio(SessionAudioChannel* channel) {
    if (channel == nullptr) {
        return;
    }
    std::lock_guard lock(g_audio_park_mutex);
    channel->park();
}

bool wait_emulator_running(SessionRuntime& runtime, int settle_attempts) {
    for (int i = 0; i < settle_attempts && runtime.emulator_running(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return runtime.emulator_running();
}

std::string format_emulator_exit_summary(int code) {
    if (code < 0) {
        return "emulator exited (unknown status)";
    }
    // PosixRetroArchProcess encodes WIFSIGNALED as 128 + signo.
    if (code >= 128 && code < 192) {
        const int sig = code - 128;
        std::ostringstream oss;
        oss << "emulator exited by signal " << sig;
        if (const char* name = strsignal(sig); name != nullptr) {
            oss << " (" << name << ")";
        }
        oss << " [code " << code << "]";
        return oss.str();
    }
    return "emulator exited (code " + std::to_string(code) + ")";
}

void start_emulator_and_verify(
    SessionRuntime& runtime,
    EmulatorStartFailDetail fail_detail) {
    runtime.start_emulator();
    if (wait_emulator_running(runtime)) {
        return;
    }

    const auto code = runtime.last_exit_code().value_or(127);
    const auto stderr_tail = runtime.last_stderr_tail();
    const auto exit_summary = format_emulator_exit_summary(code);
    std::string message;
    if (fail_detail == EmulatorStartFailDetail::DirectCli) {
        if (runtime.launch_config().standalone) {
            message =
                "Standalone " + exit_summary + " immediately. "
                "Check Ryujinx/Yuzu install and keys under ~/.local/share/archstreamer/ "
                "(ryujinx/ or yuzu/) and per-user data under the save profile Switch dirs.";
        } else {
            message =
                "RetroArch " + exit_summary + " immediately. "
                "Common causes: missing BIOS/firmware under ~/.config/retroarch/system "
                "(PS2 needs files in system/pcsx2/bios), a broken core, or RetroArch not runnable.";
        }
    } else {
        message = runtime.launch_config().standalone
            ? ("Standalone " + exit_summary + " immediately")
            : ("RetroArch " + exit_summary + " immediately");
    }
    if (!stderr_tail.empty()) {
        message += "\n\n" + stderr_tail;
    }
    throw std::runtime_error(message);
}

SessionLoopCadence::SessionLoopCadence(
    LocalControllerBridge* bridge,
    InputRouter* router,
    StreamingAudioSink* audio,
    std::optional<int> audio_slot_index,
    bool audio_enabled,
    SessionAudioChannel* audio_channel)
    : bridge_(bridge)
    , router_(router)
    , audio_(audio)
    , audio_slot_index_(audio_slot_index)
    , audio_channel_(audio_channel)
    , audio_enabled_(audio_enabled)
    , next_audio_park_(std::chrono::steady_clock::now()) {}

void SessionLoopCadence::tick() {
    if (bridge_ != nullptr && router_ != nullptr) {
        bridge_->update(*router_);
    }
    if (audio_enabled_) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= next_audio_park_) {
            if (audio_channel_ != nullptr) {
                park_session_game_audio(audio_channel_);
            } else {
                park_session_game_audio(audio_, audio_slot_index_);
            }
            next_audio_park_ = now + std::chrono::seconds(3);
        }
    }
    if (bridge_ != nullptr) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } else {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

void post_emulator_start_warmup(
    MediaServer* media,
    HostAppConfig& config,
    std::vector<MediaClientStream>& streams,
    SessionRuntime& runtime,
    StreamingAudioSink* audio,
    std::optional<int> audio_slot_index,
    VirtualGamepadBus& pads,
    RetroArchPort players,
    bool pulse_input,
    VirtualKeyboard* keyboard,
    bool gamescope_capture,
    const std::string& preferred_display,
    std::string_view log_prefix,
    SessionAudioChannel* audio_channel) {
    start_deferred_gamescope_video_if_needed(
        media,
        config,
        streams,
        runtime.emulator().process_id().value_or(0));

    if (gamescope_capture && keyboard != nullptr) {
        plug_gamescope_virtual_keyboard_after_start(
            *keyboard,
            preferred_display,
            runtime.emulator().process_id(),
            log_prefix);
    }

    if (config.audio) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        if (const auto pid = runtime.emulator().process_id(); pid.has_value()) {
            if (audio_channel != nullptr) {
                audio_channel->set_emulator_pid(*pid);
            } else if (audio != nullptr) {
                audio->track_emulator_process(
                    *pid,
                    audio_slot_index.value_or(-1));
            }
        }
        if (audio_channel != nullptr) {
            park_session_game_audio(audio_channel);
        } else {
            park_session_game_audio(audio, audio_slot_index);
        }
    }

    if (pulse_input && players > 0) {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        pulse_virtual_pad_a(pads);
    }
}

void stop_session_runtime(
    std::unique_ptr<SessionRuntime>& runtime,
    bool reset) {
    if (runtime == nullptr) {
        return;
    }
    runtime->stop_emulator();
    if (reset) {
        runtime.reset();
    }
}

void untrack_session_audio(
    StreamingAudioSink* audio,
    std::optional<int> audio_slot_index) {
    if (audio == nullptr) {
        return;
    }
    audio->untrack_emulator_process(audio_slot_index.value_or(-1));
}

void unplug_session_keyboard(VirtualKeyboard* keyboard) {
    if (keyboard == nullptr) {
        return;
    }
    keyboard->unplug();
}

void stop_session_media(std::unique_ptr<MediaServer>& media) {
    if (media == nullptr) {
        return;
    }
    media->stop();
    media.reset();
}

} // namespace archstreamer
