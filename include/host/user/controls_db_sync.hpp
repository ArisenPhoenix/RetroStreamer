#pragma once

#include "common/protocol.hpp"
#include "common/serialization.hpp"

#include <filesystem>
#include <string>
#include <string_view>

namespace archstreamer {

/**
 * Handle ControlsDbPull / ControlsDbPush for an authenticated client.
 * Uses @claimed_username (LobbyPresence / session hello) for the profile path.
 * Returns a serialized response/ack packet, or empty on type mismatch.
 */
ByteBuffer handle_controls_db_packet(
    const std::filesystem::path& save_root,
    std::string_view claimed_username,
    const PacketPayload& payload);

} // namespace archstreamer
