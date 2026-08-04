#pragma once

#include "host/save_manager.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace archstreamer {

/** One live host session writing a user's saves (for the Users browser). */
struct ActiveSaveSession {
    std::string username;
    std::string game_id;
    std::string system_key;
    std::string display_name;
    std::string content_path;
    int slot_index = -1;
};

/**
 * Authenticated control client with an open TCP connection.
 * Includes multiplayer lobby waiters and in-session players/viewers.
 * slot_index < 0 means lobby (not yet in a live slot).
 */
struct ConnectedClientPresence {
    std::string username;
    std::uint32_t client_id = 0;
    int slot_index = -1;
    std::string game_id;
    /** "lobby" or "session". */
    std::string phase;
    bool seated = false;
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
 * Ask a live slot to stop (Users tab Kick). Host polls via
 * take_active_session_stop_request and ends the session with normal teardown.
 */
void request_active_session_stop(
    const std::filesystem::path& save_root,
    int slot_index,
    std::string_view reason = "kicked");

/**
 * If a stop was requested for this slot, remove the marker and return the reason.
 * Call from the session slot loop (same process that owns the emulator).
 */
std::optional<std::string> take_active_session_stop_request(
    const std::filesystem::path& save_root,
    int slot_index);

void publish_connected_client(
    const std::filesystem::path& save_root,
    const ConnectedClientPresence& client);

void clear_connected_client(
    const std::filesystem::path& save_root,
    std::uint32_t client_id,
    int slot_index);

/** Drop every connected-* marker for a slot (or lobby when slot_index < 0). */
void clear_connected_clients_for_slot(
    const std::filesystem::path& save_root,
    int slot_index);

std::vector<ConnectedClientPresence> list_connected_clients(
    const std::filesystem::path& save_root);

/** Request the host to drop this control connection (not a blacklist). */
void request_connected_client_disconnect(
    const std::filesystem::path& save_root,
    std::uint32_t client_id,
    int slot_index,
    std::string_view reason = "kicked");

/**
 * If a disconnect was requested for this client, remove the marker and return the reason.
 */
std::optional<std::string> take_connected_client_disconnect_request(
    const std::filesystem::path& save_root,
    std::uint32_t client_id,
    int slot_index);

/**
 * True when this save row is the game the user is currently playing.
 * Prefers GameMetaStore identity (catalog id / title-id / stem); falls back to
 * exact content-path stem/filename equality when meta has no row yet.
 */
bool save_entry_is_active(
    const std::string& username,
    const std::string& system_key,
    const std::string& display_name,
    const std::filesystem::path& primary_path,
    const ActiveSaveSession& active,
    std::string_view game_key = {});

/**
 * Pick the save-browser row that should show Active for this live session.
 * Prefers meta-resolved identity matches (including PS2 user+game rows); if none,
 * exact path match; if still none, the sole save for that user+system.
 * Legacy PS2 memcard filenames never receive Active.
 */
std::optional<std::string> best_active_game_key(
    const std::vector<SaveGameEntry>& games,
    const ActiveSaveSession& active);

} // namespace archstreamer
