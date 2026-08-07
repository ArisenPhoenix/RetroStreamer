#pragma once

#include "common/game_identity.hpp"
#include "host/game_catalog.hpp"
#include "host/libretro_core_registry.hpp"

#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace archstreamer {

std::string display_name_from_path(const std::filesystem::path& content_path);
std::string lower_string(std::string value);
std::string normalized_extension(const std::filesystem::path& path);
bool path_contains_component(const std::filesystem::path& path, std::initializer_list<std::string_view> names);
bool extension_in(std::string_view extension, std::initializer_list<std::string_view> allowed);
std::optional<std::string> infer_system_key_from_path(const std::filesystem::path& content_path);
void replace_all(std::string& value, std::string_view from, std::string_view to);
std::string fold_common_latin_accents(std::string value);
std::string canonical_token(std::string value);
/**
 * Map long / legacy Meta system_key spellings onto registry keys
 * (e.g. game-boy-advance → gba). Call after canonical_token.
 */
std::string normalize_catalog_system_key(std::string system_key);
std::string sanitize_game_display_name(std::string name);
/**
 * Strip trailing region/revision tags: "Pokemon Ruby (USA, Europe) (Rev 2)" → "Pokemon Ruby".
 */
std::string strip_trailing_parenthetical_tags(std::string name);
/**
 * Fold accents, strip "Version", strip region tags, lower-case — for matching save stems
 * like "Pokémon Ruby Version.srm" to catalog "Pokemon Ruby (USA, Europe) (Rev 2)".
 */
std::string save_match_base_name(std::string name);
std::filesystem::path default_metadata_root_for(const std::filesystem::path& content_root);
std::filesystem::path metadata_path_for(
    const std::filesystem::path& content_root,
    const std::filesystem::path& metadata_root,
    const std::filesystem::path& content_path);
/**
 * Existing Meta for a ROM: Games→Meta mirror when present, else adjacent `.json`.
 * Empty if neither exists.
 */
std::filesystem::path resolve_existing_rom_meta(
    const std::filesystem::path& content_root,
    const std::filesystem::path& metadata_root,
    const std::filesystem::path& content_path);
std::uint64_t file_update_time(const std::filesystem::path& path);
std::uint64_t game_update_time(
    const std::filesystem::path& content_path,
    const std::filesystem::path& metadata_path);
/** Portable last-write time as unix epoch seconds (for game_meta.updated_at). */
std::int64_t file_mtime_unix_seconds(const std::filesystem::path& path);
std::int64_t game_mtime_unix_seconds(
    const std::filesystem::path& content_path,
    const std::filesystem::path& metadata_path = {});
/**
 * Birth time when the filesystem provides it; otherwise last-write time.
 * Used for game_meta.created_at from the Meta directory.
 */
std::int64_t file_birth_or_mtime_unix_seconds(const std::filesystem::path& path);
void apply_game_metadata(GameInfo& info, const std::filesystem::path& metadata_path);
void finalize_game_identity(GameInfo& info);
std::vector<std::string> parse_m3u_member_basenames(const std::filesystem::path& m3u_path);

/** Why a ROM was locked out of the live catalog (not hosted / not playable). */
enum class CatalogScanIssueKind {
    MissingMeta,
    StemMismatch,
    InvalidM3m,
};

struct CatalogScanIssue {
    CatalogScanIssueKind kind = CatalogScanIssueKind::MissingMeta;
    std::filesystem::path content_path;
    /** Short reason for host log / stderr. */
    std::string message;
};

/**
 * Scan ROMs under content_root. Titles without Meta, or whose filename stem does
 * not match catalog_rom_stem_for(display_name, version) from that Meta, are
 * skipped (locked) and reported in @issues_out when non-null.
 */
GameCatalog scan_game_catalog(
    const std::filesystem::path& content_root,
    const LibretroCoreRegistry& core_registry = LibretroCoreRegistry::ubuntu_defaults(),
    std::filesystem::path metadata_root = {},
    std::vector<CatalogScanIssue>* issues_out = nullptr);

} // namespace archstreamer
