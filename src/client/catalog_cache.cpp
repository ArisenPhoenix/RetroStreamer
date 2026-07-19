#include "client/catalog_cache.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <string>

namespace archstreamer {

std::filesystem::path default_catalog_cache_path() {
    if (const char* cache_home = std::getenv("XDG_CACHE_HOME"); cache_home != nullptr && cache_home[0] != '\0') {
        return std::filesystem::path{cache_home} / "archstreamer" / "catalog.json";
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
        return std::filesystem::path{home} / ".cache" / "archstreamer" / "catalog.json";
    }
    return std::filesystem::temp_directory_path() / "archstreamer-catalog.json";
}

nlohmann::json game_info_to_json(const GameInfo& game) {
    return nlohmann::json{
        {"id", game.id},
        {"identity_key", game.identity_key},
        {"asset_key", game.asset_key},
        {"display_name", game.display_name},
        {"system_name", game.system_name},
        {"system_key", game.system_key},
        {"core_name", game.core_name},
        {"canonical_name", game.canonical_name},
        {"version", game.version},
        {"language", game.language},
        {"region", game.region},
        {"supports_singleplayer", game.supports_singleplayer},
        {"supports_multiplayer", game.supports_multiplayer},
        {"min_players", game.min_players},
        {"max_players", game.max_players},
        {"updated_at", game.updated_at},
    };
}

GameInfo game_info_from_json(const nlohmann::json& json) {
    return GameInfo{
        json.at("id").get<std::string>(),
        json.value("identity_key", ""),
        json.value("asset_key", ""),
        json.at("display_name").get<std::string>(),
        json.at("system_name").get<std::string>(),
        json.value("system_key", ""),
        json.at("core_name").get<std::string>(),
        json.value("canonical_name", ""),
        json.value("version", "unknown"),
        json.value("language", "en"),
        json.value("region", "unknown"),
        json.value("supports_singleplayer", true),
        json.value("supports_multiplayer", true),
        static_cast<std::uint8_t>(json.value("min_players", 1)),
        static_cast<std::uint8_t>(json.value("max_players", MaxPlayersPerClient)),
        json.value<std::uint64_t>("updated_at", 0),
    };
}

GameList load_catalog_cache(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file) {
        return {};
    }

    const auto json = nlohmann::json::parse(file);
    if (json.value("schema_version", 0) != CatalogCacheSchemaVersion) {
        return {};
    }

    auto list = GameList{};
    list.catalog_revision = json.value<std::uint64_t>("catalog_revision", 0);
    list.full = true;
    for (const auto& game_json : json.value("games", nlohmann::json::array())) {
        list.games.push_back(game_info_from_json(game_json));
    }
    return list;
}

void save_catalog_cache(const std::filesystem::path& path, const GameList& list) {
    std::filesystem::create_directories(path.parent_path());
    auto json = nlohmann::json{
        {"schema_version", CatalogCacheSchemaVersion},
        {"catalog_revision", list.catalog_revision},
        {"games", nlohmann::json::array()},
    };
    for (const auto& game : list.games) {
        json["games"].push_back(game_info_to_json(game));
    }

    std::ofstream file(path, std::ios::trunc);
    if (!file) {
        throw std::runtime_error("failed to write catalog cache: " + path.string());
    }
    file << json.dump(2) << '\n';
}

void merge_catalog_delta(GameList& cache, const GameList& update) {
    if (update.full) {
        cache = update;
        cache.full = true;
        return;
    }

    for (const auto& deleted_id : update.deleted_game_ids) {
        cache.games.erase(
            std::remove_if(cache.games.begin(), cache.games.end(), [&](const GameInfo& game) {
                return game.id == deleted_id;
            }),
            cache.games.end());
    }

    for (const auto& changed_game : update.games) {
        const auto existing = std::find_if(cache.games.begin(), cache.games.end(), [&](const GameInfo& game) {
            return game.id == changed_game.id;
        });
        if (existing == cache.games.end()) {
            cache.games.push_back(changed_game);
        } else {
            *existing = changed_game;
        }
    }

    cache.catalog_revision = update.catalog_revision;
    cache.full = true;
}

} // namespace archstreamer
