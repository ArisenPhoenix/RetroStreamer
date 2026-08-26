#pragma once

#include "host/console/save_profile.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace archstreamer {

/**
 * Global Switch DLC system folder: <DLC>/Switch/
 * (NSPs live under per-game subdirs; this is the system parent.)
 */
std::filesystem::path switch_title_updates_directory();

/**
 * Ensure global catalog DLC for a Switch title and point Ryujinx at it.
 * Canonical layout is only <DLC>/Switch/<game_id_leaf>/ (NSPs, registered/,
 * manifest.json). Stem folders and flat SwitchUpdates are ingested into that
 * leaf once; launch never reads them as a second path.
 * Then: unpack listed NSPs into …/registered/, write
 * games/<title_id>/{updates.json,dlc.json} so CLI XCI launch applies the
 * selected patch/DLC, and symlink bis/user/Contents/registered there.
 */
void ensure_ryujinx_catalog_addons(
    const SaveProfile& save_profile,
    const std::filesystem::path& ryujinx_data_root,
    std::string_view game_id,
    std::string_view content_stem = {},
    std::string_view title_id = {});

/**
 * Legacy: unpack every NSP under DLC/Switch (and nested game dirs) into the
 * profile registered/ folder. Prefer ensure_ryujinx_catalog_addons for catalog launches.
 */
void ensure_ryujinx_title_updates(const std::filesystem::path& ryujinx_data_root);

} // namespace archstreamer
