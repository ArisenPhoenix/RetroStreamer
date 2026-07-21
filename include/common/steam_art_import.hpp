#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace archstreamer {

struct GameArtImportTarget {
    std::string asset_key;
    std::string display_name;
    std::string canonical_name;
    std::filesystem::path content_path;
};

struct SteamShortcutEntry {
    std::uint32_t appid = 0;
    std::string appname;
    std::string exe;
    std::filesystem::path content_path;
};

struct SteamArtImportOptions {
    std::filesystem::path steam_config_dir;
    // Force replace even when content is identical.
    bool overwrite = false;
    // Replace when size/content differs; skip identical files. Default for refresh.
    bool replace_when_different = true;
    bool dry_run = false;
};

struct SteamArtImportResult {
    std::size_t shortcuts_read = 0;
    std::size_t matched_games = 0;
    std::size_t files_copied = 0;
    std::size_t files_replaced = 0;
    std::size_t files_skipped = 0;
    std::vector<std::string> unmatched_shortcuts;
};

// Prefer ~/.local/share/Steam/userdata/<id>/config with a populated grid/.
std::optional<std::filesystem::path> discover_steam_config_dir();

std::vector<SteamShortcutEntry> parse_steam_shortcuts(const std::filesystem::path& shortcuts_vdf);

// Copy Steam grid images into Art/<asset_key>/{boxart,grid,hero,logo,icon}.png
SteamArtImportResult import_steam_grid_art(
    const std::vector<GameArtImportTarget>& targets,
    const std::filesystem::path& assets_root,
    const SteamArtImportOptions& options);

} // namespace archstreamer
