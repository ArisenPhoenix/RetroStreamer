#pragma once

#include "common/sha256.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace archstreamer {

/**
 * Catalog identity + version bookkeeping.
 *
 * Contract:
 * - DB / Meta store a clean base title in display_name / name, and the version in
 *   its own field. Do not embed "(1.3.2)" or "(Rev N)" in the stored title.
 * - version "0", "1", empty, or "unknown" means the base release. Treat 0/1 as a
 *   binary "known base" flag — not a meaningful version string for filenames or UI.
 * - Versions containing "rev" are also unlabeled at the edge (UI / ROM stem): the
 *   rev lives only in the version field for bookkeeping.
 * - Meaningful later builds (e.g. 1-3-2): when sending GameList to clients,
 *   version is reduced to a display token (or "") via catalog_version_display_token
 *   (numeric → "1.3.2"; text tags → Title Case words, "special-edition" → "Special Edition").
 *   Clients only append a non-empty version to display_name — no 0/1/rev checks.
 *   Host-local UI may still call catalog_label_for on full DB version fields.
 * - On-disk ROM stem must equal catalog_rom_stem_for(display_name, version)
 *   (same as catalog_label_for). Scan skips (locks) titles missing Meta or with a
 *   mismatched stem until fixed.
 */

/** Constituent fields that compose identity_key (and thus game_id). */
struct GameIdentityParts {
    std::string system_key;
    std::string canonical_name;
    std::string version;
    std::string language;
    std::string region;
};

/**
 * Catalog identity: parts → identity_key → game_id.
 * identity_key is the decomposable preimage; game_id is sha256_hex(identity_key).
 * Neither should be edited directly — change the parts and recompute.
 */
struct GameIdentity {
    GameIdentityParts parts;
    std::string identity_key;
    std::string game_id;

    /** Build identity_key and game_id from parts. */
    static GameIdentity from_parts(GameIdentityParts parts);

    static GameIdentity from_parts(
        std::string_view system_key,
        std::string_view canonical_name,
        std::string_view version,
        std::string_view language,
        std::string_view region);

    /** True when identity_key equals compose(parts). */
    bool identity_matches_parts() const;

    /** True when game_id equals sha256_hex(identity_key). */
    bool game_id_matches_identity() const;

    /** Both composition and hash checks pass. */
    bool matches() const {
        return identity_matches_parts() && game_id_matches_identity();
    }
};

/** Compose the canonical multiline identity_key from its parts. */
std::string identity_key_for(
    std::string_view system_key,
    std::string_view canonical_name,
    std::string_view version,
    std::string_view language,
    std::string_view region);

/** Parse a composed identity_key back into parts (nullopt if malformed). */
std::optional<GameIdentityParts> parse_identity_key(std::string_view identity_key);

/**
 * True when version is missing / legacy unset (normalize these to "0").
 */
bool catalog_version_is_unspecified(std::string_view version);
/**
 * True for the original-release flag: unspecified, "0", or "1".
 * Does not include rev builds — use catalog_version_is_unlabeled for edge naming.
 */
bool catalog_version_is_base(std::string_view version);
/** True when version mentions a revision (rev / revision), case-insensitive. */
bool catalog_version_contains_rev(std::string_view version);
/**
 * True when edge surfaces (UI label, ROM stem) should use the bare display_name:
 * base (0/1/…) or any rev-bearing version.
 */
bool catalog_version_is_unlabeled(std::string_view version);
/** Map unspecified → "0"; leave other values unchanged (caller may tokenize). */
std::string catalog_version_normalize(std::string_view version);
/** True when both sides normalize to the same version token. */
bool same_catalog_version(std::string_view left, std::string_view right);
/**
 * Version string for display / ROM-stem suffixes.
 * Unlabeled → empty. Numeric (1-3-2) → "1.3.2". Text tags → Title Case
 * ("special-edition" → "Special Edition").
 */
std::string catalog_version_display_token(std::string_view version);

/**
 * Presentation label: clean display_name, plus " (version)" when the program
 * must distinguish builds. Prefer calling once when packaging for a client/UI.
 * Also the canonical on-disk ROM stem (see catalog_rom_stem_for).
 */
std::string catalog_label_for(std::string_view display_name, std::string_view version);
/**
 * On-disk ROM / Meta basename (no extension): same rules as catalog_label_for.
 * Unlabeled (0/1/rev*) → bare display_name; labeled → "Title (1.1)".
 */
std::string catalog_rom_stem_for(std::string_view display_name, std::string_view version);
/**
 * Save / Switch leaf key: lowercased catalog_rom_stem_for (derived, not hand-edited).
 */
std::string catalog_content_stem_for(std::string_view display_name, std::string_view version);
/**
 * Drop a trailing " (version)" from display_name when it matches @version
 * (hyphens ↔ dots / Title Case text tags). Keeps bookkeeping labels free of version clutter.
 */
void strip_matching_version_label(std::string& display_name, std::string_view version);

/** Art / identity path key: system/canonical/language/region/version. */
std::string asset_key_for(
    std::string_view system_key,
    std::string_view canonical_name,
    std::string_view language,
    std::string_view region,
    std::string_view version);

/**
 * Defaults + identity_key + game_id + asset_key from already-tokenized fields
 * (empty language → "en", empty region → "unknown", unspecified version → "0").
 */
struct CatalogIdentityFields {
    std::string system_key;
    std::string canonical_name;
    std::string version;
    std::string language;
    std::string region;
    std::string identity_key;
    std::string game_id;
    std::string asset_key;
};

CatalogIdentityFields compose_catalog_identity(
    std::string system_key,
    std::string canonical_name,
    std::string version,
    std::string language,
    std::string region);

} // namespace archstreamer
