#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
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

struct SteamAccountInfo {
    std::string account_id;
    std::filesystem::path steam_dir;
    std::filesystem::path config_dir;
    std::size_t score = 0;
};

struct SteamArtImportOptions {
    // Full path to .../userdata/<id>/config. If empty, resolved from account id / discovery.
    std::filesystem::path steam_config_dir;
    // Steam userdata account folder name (e.g. "YOUR_STEAM_ID"). Empty = auto-detect.
    std::string steam_account_id;
    // Optional Steam install root override (contains userdata/).
    std::filesystem::path steam_dir;
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
    std::string resolved_account_id;
    std::filesystem::path resolved_config_dir;
    std::vector<std::string> unmatched_shortcuts;
};

std::vector<std::filesystem::path> steam_userdata_roots(const std::filesystem::path& steam_dir_override = {});

// List Steam userdata/<id>/config accounts that look usable (have shortcuts.vdf).
std::vector<SteamAccountInfo> list_steam_accounts(const std::filesystem::path& steam_dir_override = {});

// Best account by grid art + shortcuts size. Empty account_id means auto.
std::optional<SteamAccountInfo> resolve_steam_account(
    std::string_view account_id = {},
    const std::filesystem::path& steam_dir_override = {});

// Prefer resolved account's config/, else best discovered config/.
std::optional<std::filesystem::path> discover_steam_config_dir(
    std::string_view account_id = {},
    const std::filesystem::path& steam_dir_override = {});

std::vector<SteamShortcutEntry> parse_steam_shortcuts(const std::filesystem::path& shortcuts_vdf);

// Copy Steam grid images into Art/<asset_key>/{boxart,grid,hero,logo,icon}.png
SteamArtImportResult import_steam_grid_art(
    const std::vector<GameArtImportTarget>& targets,
    const std::filesystem::path& assets_root,
    const SteamArtImportOptions& options);

} // namespace archstreamer
