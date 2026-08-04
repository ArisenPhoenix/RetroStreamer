#pragma once

#include "common/protocol.hpp"
#include "host/save_profile.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace archstreamer {

/**
 * Host-side save browser: list / create / delete user profiles and the game
 * saves under them (Switch title leaves, PS2 memcards, RetroArch/melonDS files).
 *
 * Paths are relative to a save root (default ~/.local/share/archstreamer/saves).
 * The reserved "template" directory is never listed or deleted.
 */
struct SaveNameHints {
    /** Lowercased file/dir stem → (system_key, display_name). */
    std::unordered_map<std::string, std::pair<std::string, std::string>> by_stem;
};

struct SaveGameEntry {
    std::string username;
    std::string system_key;   // switch, ps2, nds, gba, …
    std::string system_label; // human label
    /** Stable delete key: "switch:<leaf>", "ps2:<file>", "file:<rel-under-saves>". */
    std::string game_key;
    std::string display_name;
    std::filesystem::path primary_path;
    std::uint64_t bytes = 0;
};

std::string save_system_label(std::string_view system_key);

/** Usernames with a directory under save_root (excludes template). */
std::vector<std::string> list_save_users(const std::filesystem::path& save_root);

/**
 * Enumerate game saves for one user (or all users when username empty).
 * system_filter empty → all systems. hints optional (from host catalog).
 */
std::vector<SaveGameEntry> list_save_games(
    const std::filesystem::path& save_root,
    std::string_view username = {},
    std::string_view system_filter = {},
    const SaveNameHints& hints = {});

/** Distinct system_key values present for the user (or all users). */
std::vector<std::string> list_save_systems(
    const std::filesystem::path& save_root,
    std::string_view username = {},
    const SaveNameHints& hints = {});

/** Create profile dirs + default credentials (password archstreamer, must_change). */
SaveProfile create_save_user(
    const std::filesystem::path& save_root,
    const std::string& username);

/** Delete the entire user directory. Throws if missing/invalid/template. */
void delete_save_user(
    const std::filesystem::path& save_root,
    const std::string& username);

/** Delete every save entry for this user+system_key. */
std::size_t delete_save_system(
    const std::filesystem::path& save_root,
    const std::string& username,
    std::string_view system_key,
    const SaveNameHints& hints = {});

/** Delete one game save (and companions / Switch emulator mirrors). */
void delete_save_game(
    const std::filesystem::path& save_root,
    const std::string& username,
    std::string_view game_key);

} // namespace archstreamer
