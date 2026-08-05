#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace archstreamer {

/**
 * Sibling of Games/Art/Meta: <rom_root.parent>/DLC
 * (e.g. ROMS/Games → ROMS/DLC).
 */
std::filesystem::path default_dlc_root_for(const std::filesystem::path& rom_root);

/**
 * Resolve the global DLC root:
 * 1. ARCHSTREAMER_DLC_ROOT
 * 2. default_dlc_root_for(rom_root) when rom_root is non-empty
 * 3. ARCHSTREAMER_GAMING_ROOT / ROMS/DLC
 * 4. Sibling of legacy SwitchUpdates / Games under a discovered ROMS tree
 * 5. ~/.local/share/archstreamer/system/dlc (last resort)
 */
std::filesystem::path resolve_dlc_root(const std::filesystem::path& rom_root = {});

/**
 * Games-style system folder name for DLC/<System>/… (e.g. switch → Switch).
 * Matches typical ROMS/Games/<System> directory names.
 */
std::string dlc_system_folder_name(std::string_view system_key);

/**
 * Filesystem-safe leaf for a catalog game_id (e.g. sha256:ab… → sha256_ab…).
 * Colons are invalid on Windows paths.
 */
std::string dlc_leaf_from_game_id(std::string_view game_id);

/**
 * Global per-game DLC directory keyed by catalog game_id:
 *   <dlc_root>/<System>/<game_id_leaf>/
 * System folder comes from system_key. When game_id (or system_key) changes,
 * Catalog ops rename this path. Ryujinx only consumes the nested registered/
 * NCAs via symlink — it does not care about this leaf name.
 *
 * Resolves an existing directory case-insensitively when present.
 */
std::filesystem::path catalog_dlc_game_directory(
    const std::filesystem::path& dlc_root,
    std::string_view system_key,
    std::string_view game_id);

/** Convenience: resolve_dlc_root() / Switch / <game_id_leaf>. */
std::filesystem::path switch_dlc_game_directory(std::string_view game_id);

/**
 * Legacy leaf under DLC/<System>/<content_stem>/ (pre–game_id layout).
 * Used only to migrate packs into the game_id directory.
 */
std::filesystem::path catalog_dlc_legacy_stem_directory(
    const std::filesystem::path& dlc_root,
    std::string_view system_key,
    std::string_view content_stem);

/** Legacy flat Switch UPD/DLC NSP folder (ROMS/SwitchUpdates), if present. */
std::filesystem::path legacy_switch_updates_directory();

} // namespace archstreamer
