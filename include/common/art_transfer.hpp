#pragma once

#include "common/protocol.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace archstreamer {

inline constexpr std::string_view kDisplayArtRoles[] = {
    "boxart",
    "grid",
    "icon",
    "screenshot",
    "logo",
    "hero",
};

ArtAssetResponse load_art_asset_response(
    const std::filesystem::path& art_root,
    std::string_view asset_key,
    std::string_view role);

bool art_asset_exists_locally(
    const std::filesystem::path& art_root,
    std::string_view asset_key,
    std::string_view role);

bool write_art_asset_to_cache(
    const std::filesystem::path& cache_art_root,
    const ArtAssetResponse& response);

std::filesystem::path host_art_cache_root(std::string_view host_id);
std::string sanitize_host_cache_id(std::string_view username, std::string_view address);

} // namespace archstreamer
