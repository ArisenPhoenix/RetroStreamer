#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace archstreamer {

/**
 * Resolution map (.m3m) — KEY=value (ALL_CAPS), like a small .env file.
 * Presence means richer linking (Switch title ids, etc.); absence means plain play.
 *
 * Required keys (all non-empty):
 *   TITLE_ID, ROM, PATCH_TITLE_ID, BASE
 * ROM is relative to the .m3m directory and must exist as a regular file.
 * BASE is a declaration only (no file check).
 */
struct M3mPlaylist {
    std::string title_id;
    std::string rom; // as written in the file
    std::string patch_title_id;
    std::string base;
    std::filesystem::path rom_path; // absolute/lexically_normal resolved path
};

/**
 * Parse and validate a .m3m file.
 * On failure returns nullopt and sets @error_out when non-null.
 */
std::optional<M3mPlaylist> parse_m3m_playlist(
    const std::filesystem::path& m3m_path,
    std::string* error_out = nullptr);

/** Basename of ROM= when present (for catalog hide-pass); nullopt if absent/unreadable. */
std::optional<std::string> parse_m3m_rom_basename(const std::filesystem::path& m3m_path);

} // namespace archstreamer
