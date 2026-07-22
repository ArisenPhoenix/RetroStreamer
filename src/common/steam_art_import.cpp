#include "common/steam_art_import.hpp"
#include "common/platform/paths.hpp"
#include "common/sha256.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>

namespace archstreamer {
namespace {

std::optional<std::filesystem::path> home_directory() {
    if (const auto home = user_home_directory(); !home.empty()) {
        return std::filesystem::path{home};
    }
    return std::nullopt;
}

std::string normalize_match_key(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (std::size_t index = 0; index < value.size();) {
        const auto lead = static_cast<unsigned char>(value[index]);
        if (lead == 0xC3 && index + 1 < value.size()) {
            const auto next = static_cast<unsigned char>(value[index + 1]);
            if (next == 0xA9 || next == 0x89) {
                out.push_back('e');
                index += 2;
                continue;
            }
        }
        if (lead >= 0x80) {
            ++index;
            continue;
        }
        const auto lowered = static_cast<char>(std::tolower(lead));
        if ((lowered >= 'a' && lowered <= 'z') || (lowered >= '0' && lowered <= '9')) {
            out.push_back(lowered);
        }
        ++index;
    }
    return out;
}

std::filesystem::path normalize_path_key(std::filesystem::path path) {
    path = path.lexically_normal();
    std::error_code error;
    auto canonical = std::filesystem::weakly_canonical(path, error);
    if (!error) {
        path = std::move(canonical);
    }
    return path;
}

std::string path_match_key(const std::filesystem::path& path) {
    return normalize_match_key(normalize_path_key(path).string());
}

std::vector<std::string> extract_quoted_strings(std::string_view text) {
    std::vector<std::string> values;
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (text[index] != '"') {
            continue;
        }
        const auto start = index + 1;
        auto end = start;
        while (end < text.size() && text[end] != '"') {
            ++end;
        }
        if (end >= text.size()) {
            break;
        }
        values.emplace_back(text.substr(start, end - start));
        index = end;
    }
    return values;
}

bool looks_like_rom_path(const std::filesystem::path& path) {
    if (path.empty() || !path.has_parent_path()) {
        return false;
    }
    auto extension = path.extension().string();
    if (extension.size() < 2) {
        return false;
    }
    for (char& character : extension) {
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }
    static constexpr std::string_view ignored[] = {
        ".so",
        ".dll",
        ".exe",
        ".sh",
        ".bat",
        ".appimage",
    };
    for (const auto skip : ignored) {
        if (extension == skip) {
            return false;
        }
    }
    return true;
}

std::filesystem::path content_path_from_exe(std::string_view exe) {
    const auto quoted = extract_quoted_strings(exe);
    for (auto it = quoted.rbegin(); it != quoted.rend(); ++it) {
        const auto candidate = std::filesystem::path{*it};
        if (looks_like_rom_path(candidate)) {
            return candidate;
        }
    }
    return {};
}

bool read_cstring(const std::vector<std::uint8_t>& data, std::size_t& offset, std::string& out) {
    if (offset >= data.size()) {
        return false;
    }
    const auto start = offset;
    while (offset < data.size() && data[offset] != 0) {
        ++offset;
    }
    if (offset >= data.size()) {
        return false;
    }
    out.assign(reinterpret_cast<const char*>(data.data() + start), offset - start);
    ++offset;
    return true;
}

bool read_uint32_le(const std::vector<std::uint8_t>& data, std::size_t& offset, std::uint32_t& out) {
    if (offset + 4 > data.size()) {
        return false;
    }
    out = static_cast<std::uint32_t>(data[offset])
        | (static_cast<std::uint32_t>(data[offset + 1]) << 8)
        | (static_cast<std::uint32_t>(data[offset + 2]) << 16)
        | (static_cast<std::uint32_t>(data[offset + 3]) << 24);
    offset += 4;
    return true;
}

bool skip_bytes(const std::vector<std::uint8_t>& data, std::size_t& offset, std::size_t count) {
    if (offset + count > data.size()) {
        return false;
    }
    offset += count;
    return true;
}

bool parse_vdf_object(
    const std::vector<std::uint8_t>& data,
    std::size_t& offset,
    int depth,
    SteamShortcutEntry* entry) {
    while (offset < data.size()) {
        const auto type = data[offset++];
        if (type == 0x08) {
            return true;
        }

        std::string key;
        if (!read_cstring(data, offset, key)) {
            return false;
        }

        if (type == 0x00) {
            // Nested maps (e.g. tags) are skipped; only depth-1 shortcut fields are captured.
            if (!parse_vdf_object(data, offset, depth + 1, nullptr)) {
                return false;
            }
            continue;
        }
        if (type == 0x01) {
            std::string value;
            if (!read_cstring(data, offset, value)) {
                return false;
            }
            if (entry != nullptr && depth == 1) {
                if (key == "appname" || key == "AppName") {
                    entry->appname = std::move(value);
                } else if (key == "exe" || key == "Exe") {
                    entry->exe = std::move(value);
                }
            }
            continue;
        }
        if (type == 0x02) {
            std::uint32_t value = 0;
            if (!read_uint32_le(data, offset, value)) {
                return false;
            }
            if (entry != nullptr && depth == 1 && (key == "appid" || key == "AppID")) {
                entry->appid = value;
            }
            continue;
        }
        if (type == 0x03) {
            if (!skip_bytes(data, offset, 4)) {
                return false;
            }
            continue;
        }
        if (type == 0x07) {
            if (!skip_bytes(data, offset, 8)) {
                return false;
            }
            continue;
        }
        return false;
    }
    return false;
}

std::optional<std::filesystem::path> find_grid_image(
    const std::filesystem::path& grid_dir,
    std::uint32_t appid,
    std::string_view suffix) {
    static constexpr std::string_view extensions[] = {".png", ".jpg", ".jpeg", ".webp"};
    const auto stem = std::to_string(appid) + std::string(suffix);
    for (const auto extension : extensions) {
        const auto path = grid_dir / (stem + std::string(extension));
        std::error_code error;
        if (std::filesystem::exists(path, error) && !error) {
            if (std::filesystem::is_regular_file(path, error) && !error) {
                return path;
            }
            if (std::filesystem::is_symlink(path, error) && !error) {
                return path;
            }
        }
    }
    return std::nullopt;
}

std::optional<std::string> file_digest(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return std::nullopt;
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return sha256_hex(buffer.str());
}

bool files_identical(const std::filesystem::path& left, const std::filesystem::path& right) {
    std::error_code error;
    const auto left_size = std::filesystem::file_size(left, error);
    if (error) {
        return false;
    }
    const auto right_size = std::filesystem::file_size(right, error);
    if (error || left_size != right_size) {
        return false;
    }
    const auto left_digest = file_digest(left);
    const auto right_digest = file_digest(right);
    return left_digest.has_value() && right_digest.has_value() && *left_digest == *right_digest;
}

enum class CopyDecision {
    SkipIdentical,
    CopyNew,
    ReplaceDifferent,
};

CopyDecision decide_copy(
    const std::filesystem::path& source,
    const std::filesystem::path& destination,
    const SteamArtImportOptions& options) {
    std::error_code error;
    if (!std::filesystem::exists(destination, error) || error) {
        return CopyDecision::CopyNew;
    }
    if (options.overwrite) {
        return CopyDecision::ReplaceDifferent;
    }
    if (options.replace_when_different) {
        return files_identical(source, destination)
            ? CopyDecision::SkipIdentical
            : CopyDecision::ReplaceDifferent;
    }
    const auto source_time = std::filesystem::last_write_time(source, error);
    if (error) {
        return CopyDecision::SkipIdentical;
    }
    const auto destination_time = std::filesystem::last_write_time(destination, error);
    if (error || source_time > destination_time) {
        return CopyDecision::ReplaceDifferent;
    }
    return CopyDecision::SkipIdentical;
}

bool copy_asset(
    const std::filesystem::path& source,
    const std::filesystem::path& destination,
    const SteamArtImportOptions& options,
    SteamArtImportResult& result) {
    const auto decision = decide_copy(source, destination, options);
    if (decision == CopyDecision::SkipIdentical) {
        ++result.files_skipped;
        return false;
    }
    if (options.dry_run) {
        if (decision == CopyDecision::ReplaceDifferent) {
            ++result.files_replaced;
        } else {
            ++result.files_copied;
        }
        return true;
    }

    std::error_code error;
    std::filesystem::create_directories(destination.parent_path(), error);
    if (error) {
        return false;
    }
    if (decision == CopyDecision::ReplaceDifferent) {
        std::filesystem::remove(destination, error);
        error.clear();
    }
    std::filesystem::copy_file(
        source,
        destination,
        std::filesystem::copy_options::overwrite_existing,
        error);
    if (error) {
        return false;
    }
    if (decision == CopyDecision::ReplaceDifferent) {
        ++result.files_replaced;
    } else {
        ++result.files_copied;
    }
    return true;
}

std::size_t grid_file_count(const std::filesystem::path& config_dir) {
    const auto grid_dir = config_dir / "grid";
    std::error_code error;
    if (!std::filesystem::is_directory(grid_dir, error) || error) {
        return 0;
    }
    std::size_t count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(grid_dir, error)) {
        if (error) {
            break;
        }
        if (entry.is_regular_file(error) || entry.is_symlink(error)) {
            ++count;
        }
    }
    return count;
}

} // namespace

std::vector<std::filesystem::path> steam_userdata_roots(const std::filesystem::path& steam_dir_override) {
    std::vector<std::filesystem::path> roots;
    if (!steam_dir_override.empty()) {
        roots.push_back(steam_dir_override / "userdata");
        return roots;
    }

    const auto home = home_directory();
    if (!home.has_value()) {
        return roots;
    }
    roots.push_back(*home / ".local/share/Steam/userdata");
    roots.push_back(*home / ".steam/steam/userdata");
    roots.push_back(*home / ".var/app/com.valvesoftware.Steam/data/Steam/userdata");
    return roots;
}

std::vector<SteamAccountInfo> list_steam_accounts(const std::filesystem::path& steam_dir_override) {
    std::vector<SteamAccountInfo> accounts;
    for (const auto& userdata_root : steam_userdata_roots(steam_dir_override)) {
        std::error_code error;
        if (!std::filesystem::is_directory(userdata_root, error) || error) {
            continue;
        }
        for (const auto& account : std::filesystem::directory_iterator(userdata_root, error)) {
            if (error || !account.is_directory()) {
                continue;
            }
            const auto account_id = account.path().filename().string();
            if (account_id.empty() || account_id == "0" || !std::all_of(account_id.begin(), account_id.end(), [](unsigned char c) {
                    return std::isdigit(c) != 0;
                })) {
                continue;
            }
            const auto config_dir = account.path() / "config";
            const auto shortcuts = config_dir / "shortcuts.vdf";
            if (!std::filesystem::is_regular_file(shortcuts, error) || error) {
                continue;
            }
            const auto score = grid_file_count(config_dir);
            const auto size = static_cast<std::size_t>(std::filesystem::file_size(shortcuts, error));
            SteamAccountInfo info;
            info.account_id = account_id;
            info.steam_dir = userdata_root.parent_path();
            info.config_dir = config_dir;
            info.score = score * 1000 + (error ? 0 : size);
            accounts.push_back(std::move(info));
        }
    }
    std::sort(accounts.begin(), accounts.end(), [](const SteamAccountInfo& left, const SteamAccountInfo& right) {
        if (left.score != right.score) {
            return left.score > right.score;
        }
        return left.account_id < right.account_id;
    });
    return accounts;
}

std::optional<SteamAccountInfo> resolve_steam_account(
    std::string_view account_id,
    const std::filesystem::path& steam_dir_override) {
    const auto accounts = list_steam_accounts(steam_dir_override);
    if (accounts.empty()) {
        return std::nullopt;
    }
    if (account_id.empty()) {
        return accounts.front();
    }
    for (const auto& account : accounts) {
        if (account.account_id == account_id) {
            return account;
        }
    }

    // Explicit id may exist without shortcuts yet; still resolve the path.
    for (const auto& userdata_root : steam_userdata_roots(steam_dir_override)) {
        const auto config_dir = userdata_root / std::string(account_id) / "config";
        std::error_code error;
        if (std::filesystem::is_directory(config_dir, error) && !error) {
            SteamAccountInfo info;
            info.account_id = std::string(account_id);
            info.steam_dir = userdata_root.parent_path();
            info.config_dir = config_dir;
            info.score = grid_file_count(config_dir);
            return info;
        }
    }
    return std::nullopt;
}

std::optional<std::filesystem::path> discover_steam_config_dir(
    std::string_view account_id,
    const std::filesystem::path& steam_dir_override) {
    const auto account = resolve_steam_account(account_id, steam_dir_override);
    if (!account.has_value()) {
        return std::nullopt;
    }
    return account->config_dir;
}

std::vector<SteamShortcutEntry> parse_steam_shortcuts(const std::filesystem::path& shortcuts_vdf) {
    std::ifstream input(shortcuts_vdf, std::ios::binary);
    if (!input) {
        return {};
    }
    std::vector<std::uint8_t> data(
        (std::istreambuf_iterator<char>(input)),
        std::istreambuf_iterator<char>());
    if (data.size() < 2 || data[0] != 0x00) {
        return {};
    }

    std::size_t offset = 1;
    std::string root_key;
    if (!read_cstring(data, offset, root_key) || root_key != "shortcuts") {
        return {};
    }

    std::vector<SteamShortcutEntry> entries;
    while (offset < data.size()) {
        const auto type = data[offset++];
        if (type == 0x08) {
            break;
        }
        if (type != 0x00) {
            break;
        }
        std::string index_key;
        if (!read_cstring(data, offset, index_key)) {
            break;
        }

        SteamShortcutEntry entry;
        if (!parse_vdf_object(data, offset, 1, &entry)) {
            break;
        }
        entry.content_path = content_path_from_exe(entry.exe);
        if (entry.appid != 0 || !entry.appname.empty()) {
            entries.push_back(std::move(entry));
        }
    }
    return entries;
}

SteamArtImportResult import_steam_grid_art(
    const std::vector<GameArtImportTarget>& targets,
    const std::filesystem::path& assets_root,
    const SteamArtImportOptions& options) {
    SteamArtImportResult result;
    auto config_dir = options.steam_config_dir;
    if (config_dir.empty()) {
        const auto account = resolve_steam_account(options.steam_account_id, options.steam_dir);
        if (!account.has_value()) {
            return result;
        }
        config_dir = account->config_dir;
        result.resolved_account_id = account->account_id;
        result.resolved_config_dir = account->config_dir;
    } else {
        result.resolved_config_dir = config_dir;
        result.resolved_account_id = config_dir.parent_path().filename().string();
    }

    const auto shortcuts = parse_steam_shortcuts(config_dir / "shortcuts.vdf");
    result.shortcuts_read = shortcuts.size();
    if (shortcuts.empty()) {
        return result;
    }

    const auto grid_dir = config_dir / "grid";
    std::unordered_map<std::string, const GameArtImportTarget*> by_path;
    std::unordered_map<std::string, std::vector<const GameArtImportTarget*>> by_name;
    by_path.reserve(targets.size());
    by_name.reserve(targets.size());
    for (const auto& target : targets) {
        if (!target.content_path.empty()) {
            by_path.emplace(path_match_key(target.content_path), &target);
        }
        by_name[normalize_match_key(target.display_name)].push_back(&target);
        if (!target.canonical_name.empty()) {
            by_name[normalize_match_key(target.canonical_name)].push_back(&target);
        }
    }

    for (const auto& shortcut : shortcuts) {
        const GameArtImportTarget* matched = nullptr;
        if (!shortcut.content_path.empty()) {
            if (const auto it = by_path.find(path_match_key(shortcut.content_path)); it != by_path.end()) {
                matched = it->second;
            }
        }
        if (matched == nullptr) {
            const auto name_key = normalize_match_key(shortcut.appname);
            if (const auto it = by_name.find(name_key); it != by_name.end() && it->second.size() == 1) {
                matched = it->second.front();
            }
        }
        if (matched == nullptr) {
            result.unmatched_shortcuts.push_back(
                shortcut.appname.empty() ? std::to_string(shortcut.appid) : shortcut.appname);
            continue;
        }

        ++result.matched_games;
        const auto destination_dir = assets_root / matched->asset_key;
        const struct Mapping {
            std::string_view suffix;
            const char* filename;
        } mappings[] = {
            {"p", "boxart.png"},
            {"", "grid.png"},
            {"_hero", "hero.png"},
            {"_logo", "logo.png"},
            {"_icon", "icon.png"},
        };

        for (const auto& mapping : mappings) {
            const auto source = find_grid_image(grid_dir, shortcut.appid, mapping.suffix);
            if (!source.has_value()) {
                continue;
            }
            copy_asset(*source, destination_dir / mapping.filename, options, result);
        }
    }

    return result;
}

} // namespace archstreamer
