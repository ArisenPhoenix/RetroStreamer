#pragma once

#include "host/game_catalog.hpp"
#include "host/libretro_core_registry.hpp"

#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>

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
std::string identity_key_for(
    std::string_view system_key,
    std::string_view canonical_name,
    std::string_view version,
    std::string_view language,
    std::string_view region);
std::string asset_key_for(
    std::string_view system_key,
    std::string_view canonical_name,
    std::string_view language,
    std::string_view region,
    std::string_view version);
/**
 * Drop Nintendo-style trailing "Version" from display names:
 * "Pokemon Scarlet Version" → "Pokemon Scarlet",
 * "Pokemon Black Version 2" → "Pokemon Black 2".
 * Does not strip dotted build numbers (e.g. "Sword 1.3.2") or other numeric noise.
 */
std::string sanitize_game_display_name(std::string name);
std::filesystem::path default_metadata_root_for(const std::filesystem::path& content_root);
std::filesystem::path metadata_path_for(
    const std::filesystem::path& content_root,
    const std::filesystem::path& metadata_root,
    const std::filesystem::path& content_path);
std::uint64_t file_update_time(const std::filesystem::path& path);
std::uint64_t game_update_time(
    const std::filesystem::path& content_path,
    const std::filesystem::path& metadata_path);
void apply_game_metadata(GameInfo& info, const std::filesystem::path& metadata_path);
void finalize_game_identity(GameInfo& info);
std::vector<std::string> parse_m3u_member_basenames(const std::filesystem::path& m3u_path);

GameCatalog scan_game_catalog(
    const std::filesystem::path& content_root,
    const LibretroCoreRegistry& core_registry = LibretroCoreRegistry::ubuntu_defaults(),
    std::filesystem::path metadata_root = {});

} // namespace archstreamer
