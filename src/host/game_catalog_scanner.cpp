#include "host/game_catalog_scanner.hpp"

#include "common/sha256.hpp"
#include "host/game_meta_store.hpp"
#include "host/nds/melonds_backend.hpp"
#include "host/standalone_emulator.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <climits>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace archstreamer {

std::string display_name_from_path(const std::filesystem::path& content_path) {
    return content_path.stem().string();
}

std::string trim_copy(std::string value) {
    while (!value.empty() && (value.back() == '\r' || value.back() == '\n' ||
        value.back() == ' ' || value.back() == '\t')) {
        value.pop_back();
    }
    std::size_t start = 0;
    while (start < value.size() && (value[start] == ' ' || value[start] == '\t')) {
        ++start;
    }
    return value.substr(start);
}

// Basenames of content files listed in an .m3u playlist (comments/blank lines skipped).
std::vector<std::string> parse_m3u_member_basenames(const std::filesystem::path& m3u_path) {
    std::vector<std::string> members;
    std::ifstream file(m3u_path);
    if (!file) {
        return members;
    }
    std::string line;
    while (std::getline(file, line)) {
        line = trim_copy(std::move(line));
        if (line.empty() || line.front() == '#') {
            continue;
        }
        members.push_back(std::filesystem::path(line).filename().string());
    }
    return members;
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

bool stem_looks_versioned_for_catalog(std::string_view stem) {
    static const std::regex version_token(
        R"((?:^|[\s_-])v?\d+\.\d+(?:\.\d+)?(?:$|[\s_-]))", std::regex::icase);
    return std::regex_search(std::string(stem), version_token);
}

struct InodeKey {
    std::uint64_t dev = 0;
    std::uint64_t ino = 0;
    bool operator==(const InodeKey& other) const {
        return dev == other.dev && ino == other.ino;
    }
};

struct InodeKeyHash {
    std::size_t operator()(const InodeKey& key) const {
        return (static_cast<std::size_t>(key.dev) * 1315423911u)
            ^ static_cast<std::size_t>(key.ino);
    }
};

std::optional<InodeKey> inode_key_for(const std::filesystem::path& path) {
    struct stat st {};
    if (::stat(path.c_str(), &st) != 0) {
        return std::nullopt;
    }
    if (st.st_nlink <= 1) {
        return std::nullopt;
    }
    return InodeKey{
        static_cast<std::uint64_t>(st.st_dev),
        static_cast<std::uint64_t>(st.st_ino),
    };
}

/** Prefer cleaner catalog labels when hardlinks share one ROM payload. */
bool prefer_catalog_path(
    const std::filesystem::path& candidate,
    const std::filesystem::path& incumbent) {
    const auto cand_stem = candidate.stem().string();
    const auto inc_stem = incumbent.stem().string();
    const bool cand_ver = stem_looks_versioned_for_catalog(cand_stem);
    const bool inc_ver = stem_looks_versioned_for_catalog(inc_stem);
    if (cand_ver != inc_ver) {
        return !cand_ver; // non-versioned wins
    }
    if (cand_stem.size() != inc_stem.size()) {
        return cand_stem.size() < inc_stem.size();
    }
    return candidate.string() < incumbent.string();
}

std::vector<std::filesystem::path> dedupe_hardlinked_paths(
    std::vector<std::filesystem::path> paths) {
    std::vector<std::filesystem::path> out;
    std::unordered_map<InodeKey, std::size_t, InodeKeyHash> by_inode;
    out.reserve(paths.size());
    for (auto& path : paths) {
        const auto key = inode_key_for(path);
        if (!key) {
            out.push_back(std::move(path));
            continue;
        }
        const auto it = by_inode.find(*key);
        if (it == by_inode.end()) {
            by_inode.emplace(*key, out.size());
            out.push_back(std::move(path));
            continue;
        }
        if (prefer_catalog_path(path, out[it->second])) {
            out[it->second] = std::move(path);
        }
    }
    return out;
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

std::string sanitize_game_display_name(std::string name) {
    // Trim trailing whitespace first.
    while (!name.empty() && (name.back() == ' ' || name.back() == '\t')) {
        name.pop_back();
    }
    if (name.empty()) {
        return name;
    }

    // Match trailing " Version" or " Version <digits>" (sequel), case-insensitive.
    // Leave dotted builds ("1.3.2") and other mid-name numbers alone.
    auto ends_with_ci = [](std::string_view hay, std::string_view needle) {
        if (hay.size() < needle.size()) {
            return false;
        }
        const auto tail = hay.substr(hay.size() - needle.size());
        for (std::size_t i = 0; i < needle.size(); ++i) {
            const auto a = static_cast<unsigned char>(tail[i]);
            const auto b = static_cast<unsigned char>(needle[i]);
            if (std::tolower(a) != std::tolower(b)) {
                return false;
            }
        }
        return true;
    };

    // "... Version 2" → keep the sequel digit(s).
    {
        std::size_t i = name.size();
        while (i > 0 && std::isdigit(static_cast<unsigned char>(name[i - 1]))) {
            --i;
        }
        if (i < name.size() && i > 0 && name[i - 1] == ' ') {
            const auto sequel = name.substr(i);
            const auto head = name.substr(0, i - 1);
            if (ends_with_ci(head, " Version")) {
                return head.substr(0, head.size() - std::strlen(" Version")) + " " + sequel;
            }
        }
    }

    if (ends_with_ci(name, " Version")) {
        name.resize(name.size() - std::strlen(" Version"));
        while (!name.empty() && name.back() == ' ') {
            name.pop_back();
        }
    }
    return name;
}

std::string strip_trailing_parenthetical_tags(std::string name) {
    for (;;) {
        while (!name.empty() && (name.back() == ' ' || name.back() == '\t')) {
            name.pop_back();
        }
        if (name.size() < 3 || name.back() != ')') {
            break;
        }
        const auto open = name.rfind(" (");
        if (open == std::string::npos) {
            break;
        }
        name.resize(open);
    }
    return name;
}

std::string save_match_base_name(std::string name) {
    name = fold_common_latin_accents(std::move(name));
    name = sanitize_game_display_name(std::move(name));
    name = strip_trailing_parenthetical_tags(std::move(name));
    return lower_string(std::move(name));
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
                // Prefer Meta display name for identity unless canonical_name is explicit.
                if (!metadata.contains("canonical_name")) {
                    info.canonical_name = canonical_token(info.display_name);
                }
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
    GameMetaStore meta_store;
    if (!std::filesystem::exists(content_root)) {
        return catalog;
    }
    if (metadata_root.empty()) {
        metadata_root = default_metadata_root_for(content_root);
    }

    // Collect basenames referenced by any .m3u so those disc images are hidden from
    // the catalog (the playlist entry is the playable game).
    std::unordered_set<std::string> playlist_member_basenames;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(content_root)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        if (normalized_extension(entry.path()) != "m3u") {
            continue;
        }
        for (const auto& member : parse_m3u_member_basenames(entry.path())) {
            playlist_member_basenames.insert(lower_string(member));
        }
    }

    std::size_t skipped_switch_missing_runtime = 0;
    const auto resolved_switch = resolve_switch_runtime();
    const auto resolved_melonds = resolve_melonds_runtime();

    auto resolve_core_for_path =
        [&](const std::filesystem::path& path)
        -> std::optional<std::tuple<CoreChoice, std::string, bool, std::vector<std::string>>> {
        auto system_key = infer_system_key_from_path(path);
        if (!system_key.has_value() &&
            extension_in(normalized_extension(path), {"xci", "nsp", "nsz"})) {
            system_key = "switch";
        }

        auto standalone = false;
        std::vector<std::string> standalone_args;
        std::optional<CoreChoice> core;

        if (system_key.has_value() && *system_key == "switch") {
            if (!resolved_switch.has_value()) {
                return std::nullopt;
            }
            core = CoreChoice{
                "Nintendo Switch",
                resolved_switch->display_name,
                resolved_switch->path};
            standalone = true;
            standalone_args = resolved_switch->args_before_content;
        } else if (system_key.has_value() && *system_key == "nds" && resolved_melonds.has_value()) {
            core = CoreChoice{
                "Nintendo DS",
                resolved_melonds->display_name,
                resolved_melonds->path};
            standalone = true;
            standalone_args = resolved_melonds->args_before_content;
        } else if (system_key.has_value()) {
            core = core_registry.system_core(*system_key);
        } else {
            core = core_registry.find_for_content(path);
        }
        if (!core.has_value()) {
            return std::nullopt;
        }
        return std::make_tuple(
            std::move(*core),
            system_key.value_or(canonical_token(core->system_name)),
            standalone,
            std::move(standalone_args));
    };

    std::vector<std::filesystem::path> candidate_paths;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(content_root)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        const auto basename_lower = lower_string(entry.path().filename().string());
        if (playlist_member_basenames.count(basename_lower) > 0) {
            continue;
        }
        auto system_key = infer_system_key_from_path(entry.path());
        if (system_key.has_value() && *system_key == "switch" && !resolved_switch.has_value()) {
            ++skipped_switch_missing_runtime;
            continue;
        }
        if (!resolve_core_for_path(entry.path()).has_value()) {
            continue;
        }
        candidate_paths.push_back(entry.path());
    }

    // One catalog row per ROM payload (hardlinks like Shield.xci ↔ Shield 1.3.2.xci).
    candidate_paths = dedupe_hardlinked_paths(std::move(candidate_paths));

    for (const auto& content_path : candidate_paths) {
        const auto resolved = resolve_core_for_path(content_path);
        if (!resolved.has_value()) {
            continue;
        }
        auto [core, system_key, standalone, standalone_args] = *resolved;

        const auto metadata_path = metadata_path_for(content_root, metadata_root, content_path);
        GameInfo info{
            {},
            {},
            {},
            display_name_from_path(content_path),
            core.system_name,
            system_key,
            core.core_name,
            canonical_token(display_name_from_path(content_path)),
        };
        info.updated_at = game_update_time(content_path, metadata_path);
        apply_game_metadata(info, metadata_path);
        finalize_game_identity(info);
        // Keep identity stable from the full ROM/meta name; only tidy the label.
        info.display_name = sanitize_game_display_name(std::move(info.display_name));

        std::vector<std::string> playlist_members;
        if (normalized_extension(content_path) == "m3u") {
            playlist_members = parse_m3u_member_basenames(content_path);
            for (const auto& member : playlist_members) {
                info.playlist_discs.push_back(std::filesystem::path(member).stem().string());
            }
            // Prefer first disc art when the playlist itself has no imported assets.
            if (!playlist_members.empty()) {
                const auto member_stem = std::filesystem::path(playlist_members.front()).stem().string();
                info.asset_key = asset_key_for(
                    info.system_key,
                    canonical_token(member_stem),
                    info.language,
                    info.region,
                    info.version);
            }
        }

        // DB owns identity for known titles; scan only discovers paths / new games.
        if (meta_store.ready()) {
            meta_store.bind_scanned_game(info, content_path);
        }

        catalog.add_game(HostedGame{
            std::move(info),
            core.core_path,
            content_path,
            {},
            std::move(playlist_members),
            standalone,
            std::move(standalone_args),
        });
    }

    if (skipped_switch_missing_runtime > 0) {
        std::cerr
            << "host: skipped " << skipped_switch_missing_runtime
            << " Nintendo Switch title(s): " << switch_runtime_unavailable_message() << '\n';
    }

    return catalog;
}

} // namespace archstreamer
