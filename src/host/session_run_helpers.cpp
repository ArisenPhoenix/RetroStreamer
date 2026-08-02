#include "host/session_run_helpers.hpp"

#include "host/capture_platform.hpp"
#include "host/local_controller_bridge.hpp"
#include "host/platform/default_host_platform.hpp"

#include <chrono>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace archstreamer {
namespace {

std::mutex g_audio_park_mutex;

std::string with_prefix(std::string_view prefix, std::string_view message) {
    if (prefix.empty()) {
        return std::string(message);
    }
    return std::string(prefix) + std::string(message);
}

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
    return true;
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

void start_emulator_and_verify(
    SessionRuntime& runtime,
    EmulatorStartFailDetail fail_detail) {
    runtime.start_emulator();
    for (int i = 0; i < 10 && runtime.emulator_running(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (runtime.emulator_running()) {
        return;
    }

    const auto code = runtime.last_exit_code().value_or(127);
    const auto stderr_tail = runtime.last_stderr_tail();
    std::string message;
    if (fail_detail == EmulatorStartFailDetail::DirectCli) {
        if (runtime.launch_config().standalone) {
            message =
                "Standalone emulator exited immediately (code " + std::to_string(code) + "). "
                "Check Ryujinx/Yuzu install and keys under ~/.local/share/archstreamer/ "
                "(ryujinx/ or yuzu/) and per-user data under the save profile Switch dirs.";
        } else {
            message =
                "RetroArch exited immediately (code " + std::to_string(code) + "). "
                "Common causes: missing BIOS/firmware under ~/.config/retroarch/system "
                "(PS2 needs files in system/pcsx2/bios), a broken core, or RetroArch not runnable.";
        }
    } else {
        message = runtime.launch_config().standalone
            ? "Standalone emulator exited immediately (code " + std::to_string(code) + ")"
            : "RetroArch exited immediately (code " + std::to_string(code) + ")";
    }
    if (!stderr_tail.empty()) {
        message += "\n\n" + stderr_tail;
    }
    throw std::runtime_error(message);
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
    bool pulse_input) {
    start_deferred_gamescope_video_if_needed(
        media,
        config,
        streams,
        runtime.emulator().process_id().value_or(0));

    if (config.audio) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        park_session_game_audio(audio, audio_slot_index);
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
