#pragma once

#include "host/game_meta_store.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace archstreamer {

/** Result of a Catalog-tab mutation (DB and optional filesystem side effects). */
struct CatalogOpResult {
    bool ok = false;
    std::string message;
    std::string old_game_id;
    std::string new_game_id;
    std::vector<std::string> effects;
    /** Non-zero when this mutation was written to game_meta_edits.sqlite. */
    std::int64_t edit_id = 0;
};

struct CatalogFsOptions {
    std::filesystem::path save_root;
    std::filesystem::path art_root;
    /** Global DLC root (sibling of Games); defaults empty → resolve from rom_root when set. */
    std::filesystem::path dlc_root;
    std::filesystem::path rom_root;
    /** When true, rename save stems / Switch leaves / art / DLC dirs to match field changes. */
    bool apply_filesystem = true;
};

/**
 * Recompute identity_key, game_id, and asset_key from the identity fields
 * (system_key, canonical_name, version, language, region) — same rules as catalog scan.
 * Empty canonical_name falls back to display_name before tokenization.
 */
GameMetaRecord recompute_game_meta_identity(GameMetaRecord row);

/**
 * Apply an edited game_meta row (editable columns from the Catalog UI).
 * - Recomputes identity / game_id / asset_key.
 * - If game_id changes: inserts under the new id, migrates aliases + user_games,
 *   keeps old id as a catalog_id alias, deletes the old primary row.
 * - Rebuilds standard aliases for the surviving row.
 * - Optionally renames on-disk save stems, Art/<asset_key>/, and
 *   DLC/<System>/<content_stem>/ when those change.
 *
 * game_id / identity_key / asset_key on [edited] are ignored (always recomputed).
 */
CatalogOpResult apply_game_meta_edit(
    GameMetaStore& store,
    std::string_view current_game_id,
    const GameMetaRecord& edited,
    const CatalogFsOptions& fs = {},
    std::string_view op = "edit",
    std::string_view note = {});

/**
 * Restore the "before" snapshot from a game_meta_edits row.
 * Applies filesystem renames when fs.apply_filesystem is true.
 * Records a new edit with op="rollback".
 */
CatalogOpResult rollback_game_meta_edit(
    GameMetaStore& store,
    std::int64_t edit_id,
    const CatalogFsOptions& fs = {});


/** Insert or replace one alias row. */
CatalogOpResult add_game_meta_alias(
    GameMetaStore& store,
    std::string_view game_id,
    std::string_view alias_kind,
    std::string_view alias_value,
    std::string_view system_key = {});

/** Delete one alias by primary key fields. */
CatalogOpResult delete_game_meta_alias(
    GameMetaStore& store,
    std::string_view alias_kind,
    std::string_view alias_value,
    std::string_view system_key);

/**
 * Delete a game_meta row and all of its aliases.
 * Optionally removes user_games rows for that game_id.
 * Does not delete on-disk saves/art (use Users tab / filesystem for that).
 */
CatalogOpResult delete_game_meta_entry(
    GameMetaStore& store,
    std::string_view game_id,
    bool remove_user_games = true);

/**
 * Pull common region / language tokens out of display_name and content_stem into
 * region / language fields, strip them from the names, strip trailing "Version",
 * and re-derive canonical_name from the cleaned display when requested.
 *
 * Examples:
 * - "Pokemon Ruby (USA, Europe) (Rev 2)" → display "Pokemon Ruby", region usa-europe
 *   (Rev stays in display or version — Rev 2 → version if version was unknown)
 * - language "en" found as a token may set language=en
 */
CatalogOpResult normalize_game_meta_names(
    GameMetaStore& store,
    std::string_view game_id,
    const CatalogFsOptions& fs = {},
    bool rederive_canonical_from_display = true);

} // namespace archstreamer
