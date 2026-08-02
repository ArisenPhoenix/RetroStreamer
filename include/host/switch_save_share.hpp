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
 * Ensure canonical dir exists. If a Yuzu title save has real files, migrate them in
 * with newer-wins. Returns the canonical path.
 */
std::filesystem::path ensure_canonical_switch_save(
    const SaveProfile& profile,
    std::string_view title_id);

/** Point Yuzu's nand/.../<title_id> at the canonical leaf (symlink). */
bool link_yuzu_save_to_canonical(
    const SaveProfile& profile,
    std::string_view title_id);

/**
 * Mirror Ryujinx account saves for title_id with the canonical leaf.
 *
 * LibHac uses .../save/<id>/{0,1} as a dual-bank journal. Symlinking only `0`
 * breaks Commit (it deletes/recreates that directory). This keeps `0`/`1` as
 * real directories and copies regular files newer-wins against canonical.
 */
int mirror_ryujinx_saves_with_canonical(
    const std::filesystem::path& ryujinx_bis_user_save,
    const SaveProfile& profile,
    std::string_view title_id);

/**
 * Discover title IDs from existing Yuzu nand saves and/or Ryujinx ExtraData,
 * migrate + share both emulators with canonical leaves. Returns titles synced.
 * Safe to call at session start and again after the emulator exits.
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
