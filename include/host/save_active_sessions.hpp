#pragma once

#include "host/save_manager.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace archstreamer {

/** One live host session writing a user's saves (for the Saves browser). */
struct ActiveSaveSession {
    std::string username;
    std::string game_id;
    std::string system_key;
    std::string display_name;
    std::string content_path;
    int slot_index = -1;
};

/** Per-slot status files under <save_root>/.archstreamer_active/. */
std::filesystem::path active_save_sessions_directory(const std::filesystem::path& save_root);

void publish_active_save_session(
    const std::filesystem::path& save_root,
    const ActiveSaveSession& session);

void clear_active_save_session(
    const std::filesystem::path& save_root,
    int slot_index);

std::vector<ActiveSaveSession> list_active_save_sessions(
    const std::filesystem::path& save_root);

/**
 * True when this save row is the game the user is currently playing.
 * Matches username + system, then content stem / display name (fuzzy).
 */
bool save_entry_is_active(
    const std::string& username,
    const std::string& system_key,
    const std::string& display_name,
    const std::filesystem::path& primary_path,
    const ActiveSaveSession& active);

/**
 * Pick the save-browser row that should show Active for this live session.
 * Prefers fuzzy content/display matches; if none, falls back to the sole
 * save for that user+system (covers Switch title-id leaves).
 */
std::optional<std::string> best_active_game_key(
    const std::vector<SaveGameEntry>& games,
    const ActiveSaveSession& active);

} // namespace archstreamer
