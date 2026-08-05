#pragma once

#include "host/save_profile.hpp"

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
 * Ensure global catalog DLC for a Switch title and point Ryujinx at it:
 * - <DLC>/Switch/<game_id_leaf>/manifest.json (+ NSPs nested in that folder)
 * - unpacks listed NSPs into …/registered/
 * - replaces bis/user/Contents/registered with a symlink to that directory
 * - migrates once from legacy DLC/Switch/<content_stem>/, per-user
 *   switch/addons/<stem>, and flat SwitchUpdates (NSP match uses content_stem)
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
