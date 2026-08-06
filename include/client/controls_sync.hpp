#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace archstreamer {

struct ControlsSyncResult {
    bool ok = false;
    std::string message;
    bool found = false;
    std::vector<std::uint8_t> db_bytes;
};

/**
 * One-shot TCP: GameList → LobbyPresence → ControlsDbPull → close.
 * Requires a valid profile username/password on the host.
 */
ControlsSyncResult pull_controls_db_from_host(
    const std::string& host,
    std::uint16_t control_port,
    const std::string& username,
    const std::string& password);

/**
 * One-shot TCP: GameList → LobbyPresence → ControlsDbPush → close.
 * @db_bytes must be a valid controls.sqlite pack for @username.
 */
ControlsSyncResult push_controls_db_to_host(
    const std::string& host,
    std::uint16_t control_port,
    const std::string& username,
    const std::string& password,
    const std::vector<std::uint8_t>& db_bytes);

} // namespace archstreamer
