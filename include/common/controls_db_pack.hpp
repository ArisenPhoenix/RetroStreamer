#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace archstreamer {

/** Soft cap for ControlsDbPull/Push pack payloads (same as host profile DB). */
inline constexpr std::size_t kControlsDbPackMaxBytes = 2u * 1024u * 1024u;

inline constexpr std::string_view kControlsDbPackKindButtonMap = "button_map";
inline constexpr std::string_view kControlsDbPackKindOverlayProfiles = "overlay_profiles";

struct ControlsDbPackRow {
    std::string username;
    std::string kind;
    std::string document_json;
    int version = 1;
    std::int64_t updated_at = 0;
};

/**
 * Build a standalone controls.sqlite byte array (host profile schema) containing
 * @rows (typically one username's button_map / overlay_profiles).
 */
std::vector<std::uint8_t> export_controls_db_pack(
    std::string_view username,
    const std::vector<ControlsDbPackRow>& rows,
    std::string* error_out = nullptr);

/**
 * Parse a controls.sqlite pack and return rows whose username matches
 * @expected_username (case-insensitive). Empty optional on validation failure.
 */
std::optional<std::vector<ControlsDbPackRow>> import_controls_db_pack(
    const std::vector<std::uint8_t>& bytes,
    std::string_view expected_username,
    std::string* error_out = nullptr);

} // namespace archstreamer
