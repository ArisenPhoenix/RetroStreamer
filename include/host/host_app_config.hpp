#pragma once

#include "common/participant_role.hpp"
#include "common/protocol.hpp"
#include "host/console/emulator_config.hpp"
#include "host/console/game_config.hpp"
#include "host/hardware/gpu_settings.hpp"
#include "host/virtual/pad_settings.hpp"
#include "host/video/media_config.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace archstreamer {

/**
 * Upper bound for --clients / Host tab Max clients.
 * Matches clamp_max_session_slots; independent of MaxRemoteClients (protocol).
 */
constexpr std::uint8_t MaxConcurrentSessionSlots = 4;

/**
 * God profile: module-owned knobs plus host session-management fields.
 * Session passes the nested structs into the owning buckets rather than the
 * whole config.
 */
struct HostAppConfig {
    GameConfig game;
    EmulatorConfig emulator;
    /** Module-owned knobs; session reads these instead of a flat flag bag. */
    MediaConfig media;
    PadSettings pads;
    GpuSettings gpu;

    GameSessionMode session_mode = GameSessionMode::SinglePlayer;
    std::uint8_t players = 1;
    bool list = false;
    bool list_gpus = false;
    bool dry_run = false;
    bool verbose = false;
    std::optional<std::uint16_t> control_port;
    std::optional<std::uint16_t> input_port;
    /** Max concurrent singleplayer sessions (also multiplayer lobby size). */
    std::uint8_t clients = MaxConcurrentSessionSlots;
    std::uint16_t session_timeout_seconds = 30;
    std::uint16_t client_timeout_seconds = 20;
    std::uint16_t player_reconnect_timeout_seconds = 60;
    ParticipantRole host_role = ParticipantRole::Viewer;
    std::filesystem::path save_root;
    std::filesystem::path log_root;
    std::string username;
    std::string host_name;
    /** When false, ClientHello for unknown usernames is rejected (no new save profiles). */
    bool allow_new_users = false;
    // Set only by the local GUI. Remote-spawned host_runners intentionally have
    // no owner so the Remote tab can manage them as parentless processes.
    int owner_gui_pid = 0;
};

// Serialize config to host_runner argv (without the program name). Shared by CLI round-trips
// and the GUI so flags cannot drift.
std::vector<std::string> host_app_config_to_argv(const HostAppConfig& config);

} // namespace archstreamer
