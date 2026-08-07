#include "client/catalog_blocks_cache.hpp"

#include "common/platform/paths.hpp"

#include <nlohmann/json.hpp>

#include <fstream>
#include <stdexcept>

namespace archstreamer {

std::filesystem::path default_catalog_blocks_cache_path() {
    if (const auto cache = archstreamer_cache_directory(); !cache.empty()) {
        return std::filesystem::path{cache} / "catalog_blocks.json";
    }
    return std::filesystem::temp_directory_path() / "archstreamer-catalog-blocks.json";
}

namespace {

constexpr int kBlocksCacheSchemaVersion = 1;

std::string cache_key(
    std::string_view host,
    std::uint16_t control_port,
    std::string_view username) {
    return std::string(host) + ":" + std::to_string(control_port) + ":" + std::string(username);
}

nlohmann::json load_root(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file) {
        return nlohmann::json::object({
            {"schema_version", kBlocksCacheSchemaVersion},
            {"entries", nlohmann::json::object()},
        });
    }
    try {
        auto json = nlohmann::json::parse(file);
        if (json.value("schema_version", 0) != kBlocksCacheSchemaVersion) {
            return nlohmann::json::object({
                {"schema_version", kBlocksCacheSchemaVersion},
                {"entries", nlohmann::json::object()},
            });
        }
        if (!json.contains("entries") || !json["entries"].is_object()) {
            json["entries"] = nlohmann::json::object();
        }
        return json;
    } catch (...) {
        return nlohmann::json::object({
            {"schema_version", kBlocksCacheSchemaVersion},
            {"entries", nlohmann::json::object()},
        });
    }
}

} // namespace

CatalogBlocksCache load_catalog_blocks_cache(
    const std::filesystem::path& path,
    std::string_view host,
    std::uint16_t control_port,
    std::string_view username) {
    CatalogBlocksCache out;
    if (username.empty()) {
        return out;
    }
    const auto root = load_root(path);
    const auto key = cache_key(host, control_port, username);
    const auto& entries = root.at("entries");
    if (!entries.contains(key)) {
        return out;
    }
    const auto& entry = entries.at(key);
    out.blocks_revision = entry.value<std::uint64_t>("blocks_revision", 0);
    for (const auto& id : entry.value("blocked_game_ids", nlohmann::json::array())) {
        if (id.is_string()) {
            out.blocked_game_ids.push_back(id.get<std::string>());
        }
    }
    return out;
}

void save_catalog_blocks_cache(
    const std::filesystem::path& path,
    std::string_view host,
    std::uint16_t control_port,
    std::string_view username,
    const CatalogBlocksCache& cache) {
    if (username.empty()) {
        return;
    }
    auto root = load_root(path);
    const auto key = cache_key(host, control_port, username);
    root["entries"][key] = nlohmann::json{
        {"blocks_revision", cache.blocks_revision},
        {"blocked_game_ids", cache.blocked_game_ids},
    };
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::trunc);
    if (!file) {
        throw std::runtime_error("failed to write catalog blocks cache: " + path.string());
    }
    file << root.dump(2) << '\n';
}

void merge_catalog_blocks_cache(CatalogBlocksCache& cache, const CatalogUserBlocks& update) {
    cache.blocks_revision = update.blocks_revision;
    if (update.full) {
        cache.blocked_game_ids = update.blocked_game_ids;
    }
}

} // namespace archstreamer
