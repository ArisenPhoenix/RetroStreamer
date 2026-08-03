#pragma once

#include "common/protocol.hpp"

#include <filesystem>
#include <string>
#include <string_view>

namespace archstreamer {

inline constexpr const char* GuiLogSessionMarker = "=== archstreamer_gui started ===";
inline constexpr const char* AndroidLogSessionMarker = "=== archstreamer_android started ===";

/** Return the trailing portion of `log_text` covering up to `session_count` sessions. */
std::string extract_last_log_sessions(
    std::string_view log_text,
    std::string_view session_marker,
    std::uint32_t session_count);

std::string extract_last_log_sessions_from_file(
    const std::filesystem::path& path,
    std::string_view session_marker,
    std::uint32_t session_count);

/** Write a client log dump under the host temp archstreamer-logs directory. */
std::filesystem::path save_client_log_bundle(const ClientLogBundle& bundle);

/** Handle a ClientLogBundle: save + return an ErrorPacket ack for the client. */
ErrorPacket acknowledge_client_log_bundle(const ClientLogBundle& bundle);

} // namespace archstreamer
