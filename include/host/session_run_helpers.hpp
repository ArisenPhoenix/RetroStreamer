#pragma once

#include "host/host_app_config.hpp"
#include "host/host_launch_planner.hpp"
#include "host/media_capture.hpp"
#include "host/media_server.hpp"
#include "host/session_runtime.hpp"
#include "host/streaming_audio_sink.hpp"
#include "host/virtual_gamepad.hpp"
#include "host/virtual_keyboard.hpp"

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace archstreamer {

class InputRouter;
class LocalControllerBridge;

/** Soft-fail keyboard plug: retry on virtual Xvfb/Xephyr; single try on host. */
bool plug_virtual_keyboard_with_retry(
    VirtualKeyboard& keyboard,
    bool use_virtual_capture,
    bool gamescope_capture,
    std::string_view log_prefix = {});

struct HostMediaStartRequest {
    HostAppConfig& config;
    std::string capture_display;
    VirtualDisplayBackend display_backend;
    int nvenc_cuda_device_id = -1;
    const HostMediaPlanConfig& media_config;
    const std::vector<HostMediaDestination>& destinations;
    std::vector<MediaClientStream>& streams;
};

/**
 * normalize_audio_backend + make_host_media_server + start when A/V enabled.
 * Returns nullptr when neither audio nor video is requested.
 */
std::unique_ptr<MediaServer> start_host_media_server_if_needed(
    const HostMediaStartRequest& req);

enum class EmulatorStartFailDetail {
    Brief,
    DirectCli,
};

/** Wait settle_attempts × 50ms; true if emulator is still running. */
bool wait_emulator_running(SessionRuntime& runtime, int settle_attempts = 10);

void start_emulator_and_verify(
    SessionRuntime& runtime,
    EmulatorStartFailDetail fail_detail);

/**
 * Deferred gamescope video latch, 500ms audio re-park, optional A-pulse.
 * audio_slot_index: nullopt → park_game_audio(); set → park_game_audio_for_slot.
 */
void post_emulator_start_warmup(
    MediaServer* media,
    HostAppConfig& config,
    std::vector<MediaClientStream>& streams,
    SessionRuntime& runtime,
    StreamingAudioSink* audio,
    std::optional<int> audio_slot_index,
    VirtualGamepadBus& pads,
    RetroArchPort players,
    bool pulse_input);

/** Shared park for concurrent slots (mutex) or direct global park. */
void park_session_game_audio(
    StreamingAudioSink* audio,
    std::optional<int> audio_slot_index = std::nullopt);

/**
 * Per-session loop cadence: local bridge poll, periodic audio park, sleep.
 * Owns the park deadline; call tick() once per loop iteration.
 */
class SessionLoopCadence {
public:
    SessionLoopCadence(
        LocalControllerBridge* bridge,
        InputRouter* router,
        StreamingAudioSink* audio,
        std::optional<int> audio_slot_index,
        bool audio_enabled);

    void tick();

private:
    LocalControllerBridge* bridge_ = nullptr;
    InputRouter* router_ = nullptr;
    StreamingAudioSink* audio_ = nullptr;
    std::optional<int> audio_slot_index_;
    bool audio_enabled_ = false;
    std::chrono::steady_clock::time_point next_audio_park_{};
};

void stop_session_runtime(
    std::unique_ptr<SessionRuntime>& runtime,
    bool reset = false);

void unplug_session_keyboard(VirtualKeyboard* keyboard);

void stop_session_media(std::unique_ptr<MediaServer>& media);

} // namespace archstreamer
