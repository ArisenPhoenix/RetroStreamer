#pragma once

#include "host/save_profile.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace archstreamer {

/** Canonical per-user Switch save leaf: <user>/switch/saves/<title_id>/{main,backup,...}. */
std::filesystem::path canonical_switch_save_directory(
    const SaveProfile& profile,
    std::string_view title_id);

/**
 * Ensure canonical dir exists. If a Yuzu title save has real files, migrate them in.
 * Returns the canonical path.
 */
std::filesystem::path ensure_canonical_switch_save(
    const SaveProfile& profile,
    std::string_view title_id);

/** Point Yuzu's nand/.../<title_id> at the canonical leaf (symlink). */
bool link_yuzu_save_to_canonical(
    const SaveProfile& profile,
    std::string_view title_id);

/**
 * Scan Ryujinx bis/user/save entries' ExtraData0 for title_id (user saves) and symlink
 * each matching .../0 directory to the canonical leaf.
 */
int link_ryujinx_saves_to_canonical(
    const std::filesystem::path& ryujinx_bis_user_save,
    const SaveProfile& profile,
    std::string_view title_id);

/**
 * Discover title IDs from existing Yuzu nand saves and/or Ryujinx ExtraData,
 * migrate + link both emulators to canonical leaves. Returns titles synced.
 */
std::vector<std::string> sync_switch_shared_saves(
    const SaveProfile& profile,
    const std::filesystem::path& yuzu_nand_user_save,
    const std::filesystem::path& ryujinx_bis_user_save);

/**
 * Sync using the standard per-user Yuzu/Ryujinx layout under the save profile.
 * Safe to call even when one emulator has never launched.
 */
std::vector<std::string> sync_switch_shared_saves_for_profile(const SaveProfile& profile);

/** Lowercase hex title id helper (accepts 0100... with any case). */
std::string normalize_switch_title_id(std::string_view title_id);

} // namespace archstreamer
