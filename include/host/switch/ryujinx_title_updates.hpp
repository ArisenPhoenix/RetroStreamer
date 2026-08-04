#pragma once

#include "host/save_profile.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace archstreamer {

/** Shared folder of update/DLC NSPs (Sword/Shield 1.3.2, etc.). */
std::filesystem::path switch_title_updates_directory();

/**
 * Ensure per-catalog addon registered/ contents and point Ryujinx at them:
 * - seeds switch/addons/<stem>/manifest.json once (base=empty, versioned=matching NSPs)
 * - unpacks listed NSPs into addons/<stem>/registered/
 * - replaces bis/user/Contents/registered with a symlink to that directory
 */
void ensure_ryujinx_catalog_addons(
    const SaveProfile& save_profile,
    const std::filesystem::path& ryujinx_data_root,
    std::string_view content_stem,
    std::string_view title_id = {});

/**
 * Legacy: unpack every NSP into the profile registered/ folder.
 * Prefer ensure_ryujinx_catalog_addons for catalog launches.
 */
void ensure_ryujinx_title_updates(const std::filesystem::path& ryujinx_data_root);

} // namespace archstreamer
