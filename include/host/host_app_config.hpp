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

namespace archstreamer {

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
    std::uint16_t client_timeout_seconds = 5;
    std::uint16_t player_reconnect_timeout_seconds = 60;
    ParticipantRole host_role = ParticipantRole::Player;
    bool video = false;
    std::string video_destination = "127.0.0.1";
    bool video_destination_explicit = false;
    std::uint16_t video_port = 5004;
    bool audio = false;
    std::uint16_t audio_port = 6004;
    std::string audio_source;
    AudioCaptureBackend audio_backend = AudioCaptureBackend::Pulse;
    std::string virtual_display = ":99";
    std::string video_resolution = "1280x720";
    VirtualDisplayBackend display_backend = VirtualDisplayBackend::None;
    std::optional<std::size_t> bridge_controller_index;
    std::optional<std::size_t> virtual_joypad_index;
    std::optional<std::string> ignore_controller;
    std::string retroarch_joypad_driver = "udev";
    std::filesystem::path save_root;
    std::filesystem::path art_root;
    std::string username;
};

HostMediaPlanConfig media_plan_config_for(const HostAppConfig& config);

} // namespace archstreamer
