#include "host/game_catalog_scanner.hpp"

#include "common/sha256.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <climits>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace archstreamer {

std::string display_name_from_path(const std::filesystem::path& content_path) {
    return content_path.stem().string();
}

std::string lower_string(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

std::string normalized_extension(const std::filesystem::path& path) {
    auto extension = lower_string(path.extension().string());
    if (!extension.empty() && extension.front() == '.') {
        extension.erase(extension.begin());
    }
    return extension;
}

bool path_contains_component(const std::filesystem::path& path, std::initializer_list<std::string_view> names) {
    for (const auto& part : path) {
        const auto lower = lower_string(part.string());
        for (const auto name : names) {
            if (lower == name) {
                return true;
            }
        }
    }

    return false;
}

bool extension_in(std::string_view extension, std::initializer_list<std::string_view> allowed) {
    return std::find(allowed.begin(), allowed.end(), extension) != allowed.end();
}

std::optional<std::string> infer_system_key_from_path(const std::filesystem::path& content_path) {
    const auto extension = normalized_extension(content_path);

    if (path_contains_component(content_path, {"gb", "game boy", "gameboy"}) && extension == "gb") {
        return "gb";
    }
    if (path_contains_component(content_path, {"gbc", "game boy color", "gameboy color"}) && extension == "gbc") {
        return "gbc";
    }
    if (path_contains_component(content_path, {"gba", "game boy advance", "gameboy advance"}) && extension == "gba") {
        return "gba";
    }
    if (path_contains_component(content_path, {"nds", "ds", "nintendo ds"}) && extension_in(extension, {"nds", "zip"})) {
        return "nds";
    }
    if (path_contains_component(content_path, {"3ds", "nintendo 3ds"}) && extension_in(extension, {"3ds", "cia", "cci", "cxi"})) {
        return "3ds";
    }
    if (path_contains_component(content_path, {"n64", "nintendo64", "nintendo 64"}) && extension_in(extension, {"n64", "z64", "v64"})) {
        return "n64";
    }
    if (path_contains_component(content_path, {"nes", "famicom"}) && extension_in(extension, {"nes", "fds"})) {
        return "nes";
    }
    if (path_contains_component(content_path, {"snes", "sfc", "super nintendo"}) && extension_in(extension, {"sfc", "smc"})) {
        return "snes";
    }
    if (path_contains_component(content_path, {"ps1", "psx", "playstation"}) && extension_in(extension, {"cue", "chd", "pbp", "m3u"})) {
        return "ps1";
    }
    if (path_contains_component(content_path, {"ps2", "playstation2", "playstation 2"}) && extension_in(extension, {"iso", "chd"})) {
        return "ps2";
    }
    if (path_contains_component(content_path, {"psp"}) && extension_in(extension, {"iso", "cso"})) {
        return "psp";
    }
    if (path_contains_component(content_path, {"gamecube", "gc", "ngc"}) && extension_in(extension, {"iso", "gcm", "gcz", "rvz"})) {
        return "gamecube";
    }
    if (path_contains_component(content_path, {"wii"}) && extension_in(extension, {"wbfs", "wad", "iso", "rvz"})) {
        return "wii";
    }
    if (path_contains_component(content_path, {"switch", "nx"}) && extension_in(extension, {"xci", "nsp", "nsz"})) {
        return "switch";
    }
    if (path_contains_component(content_path, {"pce", "pc engine", "turbografx", "turbografx-16"}) && extension == "pce") {
        return "pce";
    }
    if (path_contains_component(content_path, {"genesis", "megadrive", "mega drive", "sms", "game gear"}) &&
        extension_in(extension, {"gen", "smd", "sms", "gg", "sg"})) {
        return "sega-8-16";
    }

    return std::nullopt;
}

void replace_all(std::string& value, std::string_view from, std::string_view to) {
    std::string::size_type position = 0;
    while ((position = value.find(from, position)) != std::string::npos) {
        value.replace(position, from.size(), to);
        position += to.size();
    }
}

std::string fold_common_latin_accents(std::string value) {
    const auto replacements = std::vector<std::pair<std::string_view, std::string_view>>{
        {"á", "a"}, {"à", "a"}, {"â", "a"}, {"ä", "a"}, {"ã", "a"}, {"å", "a"},
        {"Á", "a"}, {"À", "a"}, {"Â", "a"}, {"Ä", "a"}, {"Ã", "a"}, {"Å", "a"},
        {"é", "e"}, {"è", "e"}, {"ê", "e"}, {"ë", "e"},
        {"É", "e"}, {"È", "e"}, {"Ê", "e"}, {"Ë", "e"},
        {"í", "i"}, {"ì", "i"}, {"î", "i"}, {"ï", "i"},
        {"Í", "i"}, {"Ì", "i"}, {"Î", "i"}, {"Ï", "i"},
        {"ó", "o"}, {"ò", "o"}, {"ô", "o"}, {"ö", "o"}, {"õ", "o"},
        {"Ó", "o"}, {"Ò", "o"}, {"Ô", "o"}, {"Ö", "o"}, {"Õ", "o"},
        {"ú", "u"}, {"ù", "u"}, {"û", "u"}, {"ü", "u"},
        {"Ú", "u"}, {"Ù", "u"}, {"Û", "u"}, {"Ü", "u"},
        {"ñ", "n"}, {"Ñ", "n"}, {"ç", "c"}, {"Ç", "c"},
    };

    for (const auto& [from, to] : replacements) {
        replace_all(value, from, to);
    }
    return value;
}

std::string canonical_token(std::string value) {
    value = fold_common_latin_accents(std::move(value));
    auto result = std::string{};
    result.reserve(value.size());
    bool last_was_separator = false;

    for (const auto character : value) {
        const auto byte = static_cast<unsigned char>(character);
        if (std::isalnum(byte)) {
            result.push_back(static_cast<char>(std::tolower(byte)));
            last_was_separator = false;
        } else if (!last_was_separator && !result.empty()) {
            result.push_back('-');
            last_was_separator = true;
        }
    }

    while (!result.empty() && result.back() == '-') {
        result.pop_back();
    }
    if (result.empty()) {
        return "unknown";
    }
    return result;
}

std::string identity_key_for(
    std::string_view system_key,
    std::string_view canonical_name,
    std::string_view version,
    std::string_view language,
    std::string_view region) {
    return
        "system=" + std::string(system_key) +
        "\nname=" + std::string(canonical_name) +
        "\nversion=" + std::string(version) +
        "\nlanguage=" + std::string(language) +
        "\nregion=" + std::string(region);
}

std::string asset_key_for(
    std::string_view system_key,
    std::string_view canonical_name,
    std::string_view language,
    std::string_view region,
    std::string_view version) {
    return
        std::string(system_key) + "/" +
        std::string(canonical_name) + "/" +
        std::string(language) + "/" +
        std::string(region) + "/" +
        std::string(version);
}

std::filesystem::path default_metadata_root_for(const std::filesystem::path& content_root) {
    return content_root.parent_path() / "Meta";
}

std::filesystem::path metadata_path_for(
    const std::filesystem::path& content_root,
    const std::filesystem::path& metadata_root,
    const std::filesystem::path& content_path) {
    auto relative = std::filesystem::relative(content_path, content_root);
    relative.replace_extension(".json");
    return metadata_root / relative;
}

std::uint64_t file_update_time(const std::filesystem::path& path) {
    std::error_code error;
    const auto time = std::filesystem::last_write_time(path, error);
    if (error) {
        return 0;
    }

    const auto count = static_cast<std::int64_t>(time.time_since_epoch().count());
    return static_cast<std::uint64_t>(count) ^ (std::uint64_t{1} << 63);
}

std::uint64_t game_update_time(
    const std::filesystem::path& content_path,
    const std::filesystem::path& metadata_path) {
    auto updated_at = file_update_time(content_path);
    if (std::filesystem::exists(metadata_path)) {
        updated_at = std::max(updated_at, file_update_time(metadata_path));
    }
    return updated_at;
}

void apply_game_metadata(GameInfo& info, const std::filesystem::path& metadata_path) {
    std::ifstream file(metadata_path);
    if (!file) {
        return;
    }

    try {
        const auto metadata = nlohmann::json::parse(file);
        if (!metadata.is_object()) {
            throw std::runtime_error("game metadata root must be a JSON object: " + metadata_path.string());
        }

        if (metadata.contains("name")) {
            auto name = metadata.at("name").get<std::string>();
            if (!name.empty()) {
                info.display_name = std::move(name);
            }
        }
        if (metadata.contains("system_name")) {
            auto system_name = metadata.at("system_name").get<std::string>();
            if (!system_name.empty()) {
                info.system_name = std::move(system_name);
            }
        }
        if (metadata.contains("system_key")) {
            auto system_key = canonical_token(metadata.at("system_key").get<std::string>());
            if (!system_key.empty()) {
                info.system_key = std::move(system_key);
            }
        }
        if (metadata.contains("canonical_name")) {
            auto canonical_name = canonical_token(metadata.at("canonical_name").get<std::string>());
            if (!canonical_name.empty()) {
                info.canonical_name = std::move(canonical_name);
            }
        }
        if (metadata.contains("version")) {
            auto version = canonical_token(metadata.at("version").get<std::string>());
            if (!version.empty()) {
                info.version = std::move(version);
            }
        }
        if (metadata.contains("language")) {
            auto language = canonical_token(metadata.at("language").get<std::string>());
            if (!language.empty()) {
                info.language = std::move(language);
            }
        }
        if (metadata.contains("region")) {
            auto region = canonical_token(metadata.at("region").get<std::string>());
            if (!region.empty()) {
                info.region = std::move(region);
            }
        }
        if (metadata.contains("modes")) {
            const auto& modes = metadata.at("modes");
            if (!modes.is_object()) {
                throw std::runtime_error("game metadata modes must be a JSON object: " + metadata_path.string());
            }
            if (modes.contains("single")) {
                info.supports_singleplayer = modes.at("single").get<bool>();
            }
            if (modes.contains("multi")) {
                info.supports_multiplayer = modes.at("multi").get<bool>();
            }
        }

        const auto apply_player_limit = [&](std::string_view field, std::uint8_t& target) {
            const auto key = std::string(field);
            if (!metadata.contains(key)) {
                return;
            }

            const auto& value = metadata.at(key);
            if (!value.is_number_unsigned() || value.get<std::uint64_t>() > UINT8_MAX) {
                throw std::runtime_error(
                    "game metadata field must be an unsigned 8-bit integer: " + key + " in " + metadata_path.string());
            }
            target = static_cast<std::uint8_t>(value.get<std::uint64_t>());
        };

        apply_player_limit("min_players", info.min_players);
        apply_player_limit("max_players", info.max_players);
    } catch (const nlohmann::json::exception& error) {
        throw std::runtime_error(
            "invalid game metadata JSON: " + metadata_path.string() + ": " + error.what());
    }

    if (!valid_game_player_limits(info.min_players, info.max_players)) {
        throw std::runtime_error("invalid game metadata player limits: " + metadata_path.string());
    }
}

void finalize_game_identity(GameInfo& info) {
    info.system_key = canonical_token(info.system_key);
    info.canonical_name = canonical_token(info.canonical_name.empty() ? info.display_name : info.canonical_name);
    info.version = canonical_token(info.version.empty() ? "unknown" : info.version);
    info.language = canonical_token(info.language.empty() ? "en" : info.language);
    info.region = canonical_token(info.region.empty() ? "unknown" : info.region);
    info.identity_key = identity_key_for(
        info.system_key,
        info.canonical_name,
        info.version,
        info.language,
        info.region);
    info.id = sha256_hex(info.identity_key);
    info.asset_key = asset_key_for(
        info.system_key,
        info.canonical_name,
        info.language,
        info.region,
        info.version);
}

GameCatalog scan_game_catalog(
    const std::filesystem::path& content_root,
    const LibretroCoreRegistry& core_registry,
    std::filesystem::path metadata_root) {
    GameCatalog catalog;
    if (!std::filesystem::exists(content_root)) {
        return catalog;
    }
    if (metadata_root.empty()) {
        metadata_root = default_metadata_root_for(content_root);
    }

    for (const auto& entry : std::filesystem::recursive_directory_iterator(content_root)) {
        if (!entry.is_regular_file()) {
            continue;
        }

        auto core = std::optional<CoreChoice>{};
        auto system_key = infer_system_key_from_path(entry.path());
        if (system_key.has_value()) {
            core = core_registry.system_core(*system_key);
        } else {
            core = core_registry.find_for_content(entry.path());
        }

        if (!core.has_value()) {
            continue;
        }

        const auto metadata_path = metadata_path_for(content_root, metadata_root, entry.path());
        GameInfo info{
            {},
            {},
            {},
            display_name_from_path(entry.path()),
            core->system_name,
            system_key.value_or(canonical_token(core->system_name)),
            core->core_name,
            canonical_token(display_name_from_path(entry.path())),
        };
        info.updated_at = game_update_time(entry.path(), metadata_path);
        apply_game_metadata(info, metadata_path);
        finalize_game_identity(info);

        catalog.add_game(HostedGame{
            std::move(info),
            core->core_path,
            entry.path(),
            {},
        });
    }

    return catalog;
}

} // namespace archstreamer
