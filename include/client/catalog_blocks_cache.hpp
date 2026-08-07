#pragma once

#include "common/protocol.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace archstreamer {

struct CatalogBlocksCache {
    std::uint64_t blocks_revision = 0;
    std::vector<GameId> blocked_game_ids;
};

std::filesystem::path default_catalog_blocks_cache_path();

/** Load cached blocks for host+username. Empty revision if missing. */
CatalogBlocksCache load_catalog_blocks_cache(
    const std::filesystem::path& path,
    std::string_view host,
    std::uint16_t control_port,
    std::string_view username);

void save_catalog_blocks_cache(
    const std::filesystem::path& path,
    std::string_view host,
    std::uint16_t control_port,
    std::string_view username,
    const CatalogBlocksCache& cache);

/** Apply a CatalogUserBlocks reply onto a local cache entry. */
void merge_catalog_blocks_cache(CatalogBlocksCache& cache, const CatalogUserBlocks& update);

} // namespace archstreamer
