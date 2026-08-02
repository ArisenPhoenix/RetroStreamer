#pragma once

#include "common/catalog_paths.hpp"
#include "common/participant_role.hpp"
#include "common/protocol.hpp"
#include "host/host_launch_planner.hpp"
#include "host/media_capture.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace archstreamer {

/** Internal-resolution multipliers for Switch standalone and RetroArch cores. */
struct ResolutionSettings {
    // Switch standalone IR (Yuzu qt-config + Ryujinx res_scale). Clamp 1–6 at write;
    // Ryujinx caps at 4 internally.
    int switch_scale = 1;
    // RetroArch IR via known cores' .opt files (1–6).
    int retroarch_scale = 1;
};

struct HostAppConfig {
    std::filesystem::path rom_root = DefaultRomRoot;
    std::filesystem::path meta_root;
    std::optional<std::string> selector;
    GameSessionMode session_mode = GameSessionMode::SinglePlayer;
    std::uint8_t players = 1;
    bool list = false;
    bool dry_run = false;
    bool pulse_input = false;
    bool verbose = false;
    std::optional<std::uint16_t> control_port;
    std::optional<std::uint16_t> input_port;
    std::uint8_t clients = 1;
    std::uint16_t session_timeout_seconds = 30;
    std::uint16_t client_timeout_seconds = 20;
    std::uint16_t player_reconnect_timeout_seconds = 60;
    ParticipantRole host_role = ParticipantRole::Viewer;
    bool video = true;
    std::string video_destination = "127.0.0.1";
    bool video_destination_explicit = false;
    std::uint16_t video_port = 5004;
    bool audio = true;
    std::uint16_t audio_port = 6004;
    std::string audio_source;
    AudioCaptureBackend audio_backend = AudioCaptureBackend::Pulse;
    std::string virtual_display = ":99";
    std::string video_resolution = "1920x1080";
    VirtualDisplayBackend display_backend = VirtualDisplayBackend::None;
    std::optional<std::size_t> bridge_controller_index;
    std::optional<std::size_t> virtual_joypad_index;
    std::optional<std::string> ignore_controller;
    std::string retroarch_joypad_driver = "udev";
    std::filesystem::path save_root;
    std::filesystem::path art_root;
    std::string username;
    // Primary GPU: H.264 nvenc encode, and game render unless separate_render_gpu.
    // "auto" or a GpuDevice::id from list_render_gpus() (e.g. nvidia:0).
    std::string encode_gpu = "auto";
    // When true, render_gpu selects the card for RetroArch/Switch GL/Vulkan (PRIME).
    bool separate_render_gpu = false;
    // Used when separate_render_gpu is true; otherwise encode_gpu is used for render too.
    std::string render_gpu = "auto";
    // Preferred API for Switch standalone. Auto = backend default (Vulkan on gamescope,
    // OpenGL on VirtualGL). Ignored for RetroArch.
    GraphicsApiPreference graphics_api = GraphicsApiPreference::Auto;
    ResolutionSettings resolution;
};

// GPU id used for RetroArch/Switch PRIME / Vulkan device selection.
inline std::string effective_render_gpu_selection(const HostAppConfig& config) {
    if (config.separate_render_gpu) {
        return config.render_gpu.empty() ? "auto" : config.render_gpu;
    }
    return config.encode_gpu.empty() ? "auto" : config.encode_gpu;
}

HostMediaPlanConfig media_plan_config_for(const HostAppConfig& config);

// Serialize config to host_runner argv (without the program name). Shared by CLI round-trips
// and the GUI so flags cannot drift.
std::vector<std::string> host_app_config_to_argv(const HostAppConfig& config);

} // namespace archstreamer
