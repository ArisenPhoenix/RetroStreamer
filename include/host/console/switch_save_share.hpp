#pragma once

#include "host/console/save_profile.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace archstreamer {

/** Lowercase hex title id helper (accepts 0100... with any case). */
std::string normalize_switch_title_id(std::string_view title_id);

/** True when value looks like a 16-hex Switch application title id (0100…). */
bool looks_like_switch_title_id(std::string_view value);

/**
 * .m3m sitting on this catalog/ROM path (the file itself, or a same-stem sibling).
 * Empty when this title is a plain ROM — identity is the catalog stem.
 */
std::filesystem::path switch_m3m_map_for_title(const std::filesystem::path& content_path);

bool switch_title_has_m3m_map(const std::filesystem::path& content_path);

/**
 * Canonical catalog-keyed save leaf:
 *   <user>/switch/saves/<content_stem>/{main,backup,...}
 * content_stem is the ROM filename stem as chosen in the catalog.
 */
std::filesystem::path canonical_catalog_switch_save_directory(
    const SaveProfile& profile,
    std::string_view content_stem);

/** Legacy title-ID leaf (pre–catalog-keyed layout). */
std::filesystem::path legacy_title_id_switch_save_directory(
    const SaveProfile& profile,
    std::string_view title_id);

/**
 * Global Switch DLC game dir: <DLC>/Switch/<game_id_leaf>/
 * (profile unused; kept for call-site compatibility).
 */
std::filesystem::path catalog_switch_addon_directory(
    const SaveProfile& profile,
    std::string_view game_id);

/**
 * Ensure the catalog stem save dir exists. If empty, claim once from the legacy
 * title-ID folder (first claimant wins). Returns the stem directory.
 */
std::filesystem::path ensure_catalog_switch_save(
    const SaveProfile& profile,
    std::string_view content_stem,
    std::string_view title_id);

/**
 * Resolve Nintendo application title id for a catalog stem.
 *
 * When uses_m3m_map is true (or content_path is/has a .m3m), this is the
 * versioned map path: TITLE_ID from the .m3m, then sidecar / NSP names /
 * leftover BIS. When the title has no .m3m, only this stem's sidecar is
 * used — the catalog name is already the identity.
 */
std::string resolve_switch_title_id_for_catalog(
    const SaveProfile& profile,
    std::string_view content_stem,
    const std::filesystem::path& content_path = {},
    bool uses_m3m_map = false);

/**
 * Pre-launch: if this user's live Ryujinx bank for title_id is newer than the
 * last stamped stem, copy it back there. Then snapshot the launched stem,
 * replace Ryujinx BIS / Yuzu from that stem, and stamp the stem after a
 * successful replace. Stamp + replace is one step; the stamp is the owner mark.
 * Returns a short status token (stem) for logging.
 */
std::string sync_catalog_switch_save_for_launch(
    const SaveProfile& profile,
    std::string_view content_stem,
    std::string_view title_id,
    bool uses_m3m_map = false);

/**
 * Post-exit: mirror Ryujinx/Yuzu account saves for title_id back into the stem only.
 */
std::string sync_catalog_switch_save_after_exit(
    const SaveProfile& profile,
    std::string_view content_stem,
    std::string_view title_id,
    bool uses_m3m_map = false);

/**
 * Profile-wide discovery only (no BIS remirror of every title into one leaf).
 * Ensures legacy title-ID dirs exist as directories; returns known title ids / stems.
 * Prefer sync_catalog_switch_save_for_launch at session start.
 */
std::vector<std::string> sync_switch_shared_saves_for_profile(const SaveProfile& profile);

/** @deprecated Prefer catalog stem APIs; kept for title-ID tooling. */
std::filesystem::path canonical_switch_save_directory(
    const SaveProfile& profile,
    std::string_view title_id);

std::filesystem::path ensure_canonical_switch_save(
    const SaveProfile& profile,
    std::string_view title_id);

bool link_yuzu_save_to_canonical(
    const SaveProfile& profile,
    std::string_view title_id);

int mirror_ryujinx_saves_with_canonical(
    const std::filesystem::path& ryujinx_bis_user_save,
    const SaveProfile& profile,
    std::string_view title_id);

std::vector<std::string> sync_switch_shared_saves(
    const SaveProfile& profile,
    const std::filesystem::path& yuzu_nand_user_save,
    const std::filesystem::path& ryujinx_bis_user_save);

} // namespace archstreamer
