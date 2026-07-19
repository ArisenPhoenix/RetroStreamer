#pragma once

#include "common/protocol.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>

namespace archstreamer {

constexpr int CatalogCacheSchemaVersion = 3;

std::filesystem::path default_catalog_cache_path();
nlohmann::json game_info_to_json(const GameInfo& game);
GameInfo game_info_from_json(const nlohmann::json& json);
GameList load_catalog_cache(const std::filesystem::path& path);
void save_catalog_cache(const std::filesystem::path& path, const GameList& list);
void merge_catalog_delta(GameList& cache, const GameList& update);

} // namespace archstreamer
