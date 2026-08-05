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
 * Global per-game DLC directory:
 *   <dlc_root>/<System>/<content_stem>/
 * content_stem matches game_meta.content_stem / the catalog ROM stem.
 */
std::filesystem::path catalog_dlc_game_directory(
    const std::filesystem::path& dlc_root,
    std::string_view system_key,
    std::string_view content_stem);

/** Convenience: resolve_dlc_root() / Switch / <content_stem>. */
std::filesystem::path switch_dlc_game_directory(std::string_view content_stem);

/**
 * Legacy flat Switch UPD/DLC NSP folder (ROMS/SwitchUpdates), if present.
 * Used only to seed/migrate into DLC/Switch/<stem>/.
 */
std::filesystem::path legacy_switch_updates_directory();

} // namespace archstreamer
