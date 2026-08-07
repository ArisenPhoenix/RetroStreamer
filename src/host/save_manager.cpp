#include "host/save_manager.hpp"

#include "archstreamer/runtime_cadence/cadence.hpp"
#include "common/game_identity.hpp"
#include "host/game_catalog_scanner.hpp"
#include "host/game_meta_store.hpp"
#include "host/ps2_memcard.hpp"
#include "host/save_active_sessions.hpp"
#include "host/switch_save_share.hpp"
#include "host/user_credentials.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <unordered_set>

namespace archstreamer {
namespace {

std::string to_lower(std::string value) {
    for (char& ch : value) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return value;
}

bool looks_like_title_id(std::string_view value) {
    if (value.size() != 16) {
        return false;
    }
    for (char ch : value) {
        if (!std::isxdigit(static_cast<unsigned char>(ch))) {
            return false;
        }
    }
    return value.rfind("0100", 0) == 0;
}

bool is_reserved_user(std::string_view username) {
    return username.empty() || username == "template";
}

std::uint64_t directory_bytes(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        return 0;
    }
    if (std::filesystem::is_regular_file(path, ec)) {
        return std::filesystem::file_size(path, ec);
    }
    std::uint64_t total = 0;
    if (!std::filesystem::is_directory(path, ec)) {
        return 0;
    }
    for (const auto& entry : std::filesystem::recursive_directory_iterator(
             path, std::filesystem::directory_options::skip_permission_denied, ec)) {
        if (entry.is_regular_file(ec)) {
            total += entry.file_size(ec);
        }
    }
    return total;
}

void remove_path_best_effort(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
}

std::optional<std::uint64_t> read_le_u64(const std::filesystem::path& path, std::size_t offset) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    in.seekg(static_cast<std::streamoff>(offset));
    std::uint64_t value = 0;
    in.read(reinterpret_cast<char*>(&value), sizeof(value));
    if (!in) {
        return std::nullopt;
    }
    return value;
}

std::string title_id_from_extra_data(const std::filesystem::path& extra_data) {
    const auto value = read_le_u64(extra_data, 0);
    if (!value.has_value()) {
        return {};
    }
    return normalize_switch_title_id(
        [&] {
            char buf[17];
            std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(*value));
            return std::string(buf);
        }());
}

std::string read_ryujinx_gui_title(
    const std::filesystem::path& user_dir,
    std::string_view title_id) {
    const auto tid = normalize_switch_title_id(title_id);
    if (tid.empty()) {
        return {};
    }
    const auto path = user_dir / "ryujinx" / "xdg-config" / "Ryujinx" / "games" / tid / "gui"
        / "metadata.json";
    std::ifstream in(path);
    if (!in) {
        return {};
    }
    try {
        const auto json = nlohmann::json::parse(in);
        auto title = json.value("title", "");
        while (!title.empty() && (title.back() == ' ' || title.back() == '\n')) {
            title.pop_back();
        }
        return title;
    } catch (...) {
        return {};
    }
}

/** Learn title-id → catalog aliases from every user's Ryujinx GUI metadata. */
void harvest_ryujinx_title_aliases(const std::filesystem::path& save_root) {
    try {
        GameMetaStore meta;
        if (!meta.ready()) {
            return;
        }
        std::error_code ec;
        for (const auto& user : list_save_users(save_root)) {
            const auto games_root =
                save_root / user / "ryujinx" / "xdg-config" / "Ryujinx" / "games";
            if (!std::filesystem::is_directory(games_root, ec)) {
                continue;
            }
            for (const auto& entry : std::filesystem::directory_iterator(games_root, ec)) {
                if (!entry.is_directory(ec)) {
                    continue;
                }
                const auto leaf = entry.path().filename().string();
                if (!looks_like_title_id(leaf)) {
                    continue;
                }
                const auto tid = normalize_switch_title_id(leaf);
                if (meta.resolve(tid, "switch").has_value()) {
                    continue;
                }
                const auto title = read_ryujinx_gui_title(save_root / user, tid);
                if (title.empty()) {
                    continue;
                }
                (void)meta.learn_switch_title_id(tid, title);
            }
        }
    } catch (...) {
        // Soft-fail.
    }
}

bool save_extension(std::string_view ext) {
    static const std::unordered_set<std::string> kExts{
        "srm", "sav", "dsv", "rtc", "mcd", "ldci", "raw", "eep", "fla", "sra"};
    return kExts.contains(std::string(ext));
}

std::string infer_system_from_parent(std::string_view parent) {
    const auto lower = to_lower(std::string(parent));
    if (lower == "gambatte" || lower == "sameboy" || lower == "doublecherrygb") {
        return "gb-gbc";
    }
    if (lower == "bsnes" || lower == "snes9x" || lower.find("snes") != std::string::npos) {
        return "snes";
    }
    if (lower == "dolphin-emu" || lower == "dolphin") {
        return "gamecube";
    }
    if (lower == "citra") {
        return "3ds";
    }
    if (lower == "melonds" || lower == "desmume") {
        return "nds";
    }
    if (lower == "ppsspp") {
        return "psp";
    }
    if (lower == "mupen64plus" || lower.find("n64") != std::string::npos) {
        return "n64";
    }
    return {};
}

std::string infer_system_from_extension(std::string_view ext) {
    if (ext == "dsv") {
        return "nds";
    }
    if (ext == "mcd") {
        return "ps1";
    }
    if (ext == "rtc") {
        return "gb-gbc";
    }
    if (ext == "raw") {
        return "gamecube";
    }
    if (ext == "sav") {
        return "nds"; // melonDS / common cartridge; catalog hints refine this
    }
    if (ext == "srm") {
        return "other";
    }
    return "other";
}

/** Users-tab grouping key: catalog keeps gb vs gbc, browser shows one "Game Boy / Color". */
std::string normalize_browser_system_key(std::string_view system_key) {
    return normalize_save_browser_system_key(system_key);
}

std::pair<std::string, std::string> resolve_file_labels(
    const std::filesystem::path& relative,
    const SaveNameHints& hints,
    GameMetaStore* meta) {
    const auto raw_stem = relative.stem().string();
    const auto stem = to_lower(raw_stem);
    const auto folded = to_lower(fold_common_latin_accents(raw_stem));
    const auto base = save_match_base_name(raw_stem);

    auto from_hints = [&](const std::string& key) -> std::optional<std::pair<std::string, std::string>> {
        if (key.empty()) {
            return std::nullopt;
        }
        if (const auto it = hints.by_stem.find(key); it != hints.by_stem.end()) {
            return it->second;
        }
        return std::nullopt;
    };
    auto from_meta = [&](const std::string& key) -> std::optional<std::pair<std::string, std::string>> {
        if (key.empty() || meta == nullptr || !meta->ready()) {
            return std::nullopt;
        }
        if (auto row = meta->resolve(key, {})) {
            return std::pair{row->system_key, row->display_name};
        }
        return std::nullopt;
    };
    auto try_key = [&](const std::string& key) -> std::optional<std::pair<std::string, std::string>> {
        if (auto hit = from_hints(key)) {
            return hit;
        }
        return from_meta(key);
    };

    if (auto hit = try_key(stem)) {
        return *hit;
    }
    if (folded != stem) {
        if (auto hit = try_key(folded)) {
            return *hit;
        }
    }
    if (!base.empty() && base != stem && base != folded) {
        if (auto hit = try_key(base)) {
            return *hit;
        }
    }

    // Prefix fallback: "pokemon ruby" → "pokemon ruby (usa, europe) (rev 2)".
    if (!base.empty()) {
        const auto paren = base + " (";
        const auto version = base + " version";
        for (const auto& [key, value] : hints.by_stem) {
            if (key == base || key.rfind(paren, 0) == 0 || key.rfind(version, 0) == 0) {
                return value;
            }
        }
    }

    // Last resort: layout/extension heuristics (not title inventing from aliases).
    const auto parent = relative.parent_path().filename().string();
    if (auto from_parent = infer_system_from_parent(parent); !from_parent.empty()) {
        return {from_parent, sanitize_game_display_name(raw_stem)};
    }
    auto ext = relative.extension().string();
    if (!ext.empty() && ext.front() == '.') {
        ext.erase(ext.begin());
    }
    return {infer_system_from_extension(to_lower(ext)), sanitize_game_display_name(raw_stem)};
}

/** Attach catalog id + stem-mismatch flags for RetroArch-style file saves. */
void annotate_file_save_catalog(
    SaveGameEntry& entry,
    std::string_view disk_stem,
    GameMetaStore* meta) {
    if (meta == nullptr || !meta->ready() || disk_stem.empty()) {
        return;
    }

    auto apply_row = [&](const GameMetaRecord& row, bool exact_stem_ok) {
        entry.catalog_game_id = row.game_id;
        entry.system_key = normalize_browser_system_key(
            row.system_key.empty() ? entry.system_key : row.system_key);
        entry.system_label = save_system_label(entry.system_key);
        if (exact_stem_ok) {
            entry.display_name =
                row.display_name.empty() ? entry.display_name : row.display_name;
            entry.save_stem_mismatch = false;
            entry.expected_save_stem.clear();
            return;
        }
        entry.save_stem_mismatch = true;
        entry.expected_save_stem = catalog_save_stem_for(row.display_name, row.version);
        // Keep the on-disk stem visible so the bad name is obvious in Users.
        if (entry.display_name.empty()) {
            entry.display_name = std::string(disk_stem);
        }
    };

    if (auto row = meta->resolve(disk_stem, entry.system_key)) {
        const bool ok = catalog_save_stem_matches(disk_stem, row->display_name, row->version);
        apply_row(*row, ok);
        return;
    }
    if (auto row = meta->resolve(disk_stem, {})) {
        const bool ok = catalog_save_stem_matches(disk_stem, row->display_name, row->version);
        apply_row(*row, ok);
        return;
    }

    // Near-miss intent only (e.g. "Title v1.1" → "Title (1.1)") — never accept as OK.
    const auto intent = catalog_normalize_save_stem_intent(disk_stem);
    if (intent != disk_stem) {
        if (auto row = meta->resolve(intent, entry.system_key)) {
            apply_row(*row, false);
            return;
        }
        if (auto row = meta->resolve(intent, {})) {
            apply_row(*row, false);
            return;
        }
        const auto intent_l = to_lower(intent);
        if (intent_l != to_lower(std::string(disk_stem))) {
            if (auto row = meta->resolve(intent_l, entry.system_key)) {
                apply_row(*row, false);
                return;
            }
            if (auto row = meta->resolve(intent_l, {})) {
                apply_row(*row, false);
                return;
            }
        }
    }
}

void append_switch_entries(
    std::vector<SaveGameEntry>& out,
    const std::string& username,
    const std::filesystem::path& user_dir,
    const SaveNameHints& hints,
    GameMetaStore* meta) {
    const auto switch_root = user_dir / "switch" / "saves";
    std::error_code ec;
    if (!std::filesystem::is_directory(switch_root, ec)) {
        return;
    }
    for (const auto& entry : std::filesystem::directory_iterator(switch_root, ec)) {
        // Friendly name symlinks are not save roots (catalog stems are real dirs).
        if (entry.is_symlink(ec)) {
            continue;
        }
        if (!entry.is_directory(ec)) {
            continue;
        }
        const auto leaf = entry.path().filename().string();
        if (leaf.empty() || leaf == "." || leaf == "..") {
            continue;
        }
        // Count only payload files (ignore .title_id / claim markers).
        std::uint64_t bytes = 0;
        for (const auto& file : std::filesystem::directory_iterator(entry.path(), ec)) {
            if (!file.is_regular_file(ec)) {
                continue;
            }
            const auto name = file.path().filename().string();
            if (!name.empty() && name.front() == '.') {
                continue;
            }
            bytes += std::filesystem::file_size(file.path(), ec);
        }
        if (bytes == 0) {
            continue;
        }
        std::string display = leaf;
        if (looks_like_title_id(leaf)) {
            const auto tid = normalize_switch_title_id(leaf);
            std::string title;
            if (const auto it = hints.by_stem.find(tid); it != hints.by_stem.end()) {
                title = it->second.second;
            } else if (meta != nullptr && meta->ready()) {
                if (const auto row = meta->resolve(tid, "switch")) {
                    title = row->display_name;
                }
            }
            if (title.empty()) {
                display = "Legacy Switch [" + tid + "]";
            } else {
                display = title + " [legacy " + tid + "]";
            }
        } else if (const auto it = hints.by_stem.find(to_lower(leaf)); it != hints.by_stem.end()) {
            display = it->second.second;
        } else if (meta != nullptr && meta->ready()) {
            if (const auto row = meta->resolve(leaf, "switch")) {
                display = row->display_name;
            } else {
                const auto folded = to_lower(fold_common_latin_accents(leaf));
                if (const auto row = meta->resolve(folded, "switch")) {
                    display = row->display_name;
                }
            }
        } else {
            const auto folded = to_lower(fold_common_latin_accents(leaf));
            if (const auto it = hints.by_stem.find(folded); it != hints.by_stem.end()) {
                display = it->second.second;
            }
        }
        out.push_back(SaveGameEntry{
            username,
            "switch",
            save_system_label("switch"),
            "switch:" + leaf,
            std::move(display),
            entry.path(),
            bytes,
        });
    }
}

void append_meta_system_entries(
    std::vector<SaveGameEntry>& out,
    const std::string& username,
    std::string_view system_key,
    GameMetaStore* meta,
    const std::unordered_set<std::string>& skip_catalog_ids);

void append_ps2_entries(
    std::vector<SaveGameEntry>& out,
    const std::string& username,
    const std::filesystem::path& user_dir,
    GameMetaStore* meta) {
    if (meta == nullptr || !meta->ready()) {
        return;
    }

    std::vector<Ps2MemcardUsage> cards;
    std::uint64_t capacity = 0;
    const auto cards_dir = user_dir / "pcsx2" / "memcards";
    for (const char* name : {"Mcd001.ps2", "Mcd002.ps2"}) {
        if (auto usage = read_ps2_memcard_usage(cards_dir / name)) {
            capacity += usage->capacity_bytes;
            cards.push_back(std::move(*usage));
        }
    }

    auto serial_for_game = [&](std::string_view game_id) -> std::string {
        for (const auto& alias : meta->list_aliases(game_id)) {
            if (alias.alias_kind == game_meta_alias::kSerial
                || alias.alias_kind == game_meta_alias::kPs2Product) {
                if (!alias.alias_value.empty()) {
                    return alias.alias_value;
                }
            }
        }
        return {};
    };

    for (const auto& played : meta->list_user_games(username, "ps2")) {
        std::string display = played.game_id;
        std::string content_stem;
        std::string serial;
        if (const auto row = meta->find_by_id(played.game_id)) {
            display = row->display_name.empty() ? row->canonical_name : row->display_name;
            if (display.empty()) {
                display = played.game_id;
            }
            content_stem = row->content_stem;
        } else if (const auto row = meta->resolve(played.game_id, "ps2")) {
            display = row->display_name;
            content_stem = row->content_stem;
        }
        serial = serial_for_game(played.game_id);

        std::uint64_t bytes = 0;
        for (const auto& card : cards) {
            if (auto hit = ps2_memcard_bytes_for_game(card, display, content_stem, serial)) {
                bytes += *hit;
            }
        }

        SaveGameEntry entry{
            username,
            "ps2",
            save_system_label("ps2"),
            ps2_meta_game_key(played.game_id),
            std::move(display),
            {},
            bytes,
        };
        entry.capacity_bytes = capacity;
        entry.catalog_game_id = played.game_id;
        out.push_back(std::move(entry));
    }
}

/** Strip punctuation/spaces for compact stem matching (PokemonXD ↔ pokemon xd - …). */
std::string compact_stem_key(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (char ch : value) {
        const auto u = static_cast<unsigned char>(ch);
        if (std::isalnum(u)) {
            out.push_back(static_cast<char>(std::tolower(u)));
        }
    }
    return out;
}

std::optional<std::pair<std::string, std::string>> resolve_dolphin_title(
    std::string_view system_key,
    std::string_view game_code,
    std::string_view save_label,
    const SaveNameHints& hints,
    GameMetaStore* meta) {
    auto try_key = [&](std::string_view key) -> std::optional<std::pair<std::string, std::string>> {
        if (key.empty()) {
            return std::nullopt;
        }
        const auto lower = to_lower(std::string(key));
        if (const auto it = hints.by_stem.find(lower); it != hints.by_stem.end()) {
            if (it->second.first.empty() || it->second.first == system_key) {
                return it->second;
            }
        }
        if (meta != nullptr && meta->ready()) {
            if (auto row = meta->resolve(key, system_key)) {
                return std::pair{row->system_key, row->display_name};
            }
            if (auto row = meta->resolve(key, {})) {
                if (row->system_key == system_key) {
                    return std::pair{row->system_key, row->display_name};
                }
            }
        }
        return std::nullopt;
    };

    if (auto hit = try_key(game_code)) {
        return hit;
    }
    if (auto hit = try_key(save_label)) {
        return hit;
    }
    const auto compact = compact_stem_key(save_label);
    if (compact.size() >= 4) {
        for (const auto& [key, value] : hints.by_stem) {
            if (!value.first.empty() && value.first != system_key) {
                continue;
            }
            const auto key_c = compact_stem_key(key);
            if (key_c.rfind(compact, 0) == 0 || compact.rfind(key_c, 0) == 0) {
                return value;
            }
        }
    }
    return std::nullopt;
}

bool read_gci_identity(
    const std::filesystem::path& path,
    std::string& game_code,
    std::string& internal_name) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    char header[0x40]{};
    in.read(header, sizeof(header));
    if (in.gcount() < 6) {
        return false;
    }
    game_code.assign(header, 4);
    for (char ch : game_code) {
        if (!std::isalnum(static_cast<unsigned char>(ch))) {
            return false;
        }
    }
    internal_name.assign(header + 0x08, 32);
    const auto nul = internal_name.find('\0');
    if (nul != std::string::npos) {
        internal_name.resize(nul);
    }
    while (!internal_name.empty()
           && (internal_name.back() == ' ' || internal_name.back() == '\0')) {
        internal_name.pop_back();
    }
    return !game_code.empty();
}

/** Parse Dolphin export name "01-GXXE-PokemonXD.gci" → comment after game code. */
std::string gci_filename_comment(const std::filesystem::path& path) {
    const auto stem = path.stem().string();
    // NN-GAME-Comment
    const auto first = stem.find('-');
    if (first == std::string::npos) {
        return {};
    }
    const auto second = stem.find('-', first + 1);
    if (second == std::string::npos || second + 1 >= stem.size()) {
        return {};
    }
    return stem.substr(second + 1);
}

void append_dolphin_gamecube_entries(
    std::vector<SaveGameEntry>& out,
    const std::string& username,
    const std::filesystem::path& user_dir,
    const SaveNameHints& hints,
    GameMetaStore* meta) {
    const auto gc_root = user_dir / "saves" / "dolphin-emu" / "User" / "GC";
    std::error_code ec;
    if (!std::filesystem::is_directory(gc_root, ec)) {
        return;
    }

    struct Group {
        std::filesystem::path primary;
        std::uint64_t bytes = 0;
        std::string game_code;
        std::string label;
        std::string catalog_game_id;
        std::string display;
    };
    std::unordered_map<std::string, Group> by_code;

    for (const auto& entry : std::filesystem::recursive_directory_iterator(
             gc_root, std::filesystem::directory_options::skip_permission_denied, ec)) {
        if (!entry.is_regular_file(ec)) {
            continue;
        }
        if (to_lower(entry.path().extension().string()) != ".gci") {
            continue;
        }
        std::string game_code;
        std::string internal_name;
        if (!read_gci_identity(entry.path(), game_code, internal_name)) {
            continue;
        }
        const auto code_key = to_lower(game_code);
        auto& group = by_code[code_key];
        group.bytes += directory_bytes(entry.path());
        if (group.primary.empty()) {
            group.primary = entry.path();
            group.game_code = game_code;
            group.label = internal_name;
            if (group.label.empty()) {
                group.label = gci_filename_comment(entry.path());
            }
            if (group.label.empty()) {
                group.label = game_code;
            }
            if (auto hit = resolve_dolphin_title(
                    "gamecube", game_code, group.label, hints, meta)) {
                group.display = hit->second;
                if (meta != nullptr && meta->ready()) {
                    if (auto row = meta->resolve(hit->second, "gamecube")) {
                        group.catalog_game_id = row->game_id;
                    } else if (auto row = meta->resolve(game_code, "gamecube")) {
                        group.catalog_game_id = row->game_id;
                    }
                }
            } else {
                group.display = group.label;
            }
            if (group.catalog_game_id.empty() && meta != nullptr && meta->ready()) {
                if (auto row = meta->resolve(game_code, "gamecube")) {
                    group.catalog_game_id = row->game_id;
                    if (!row->display_name.empty()) {
                        group.display = row->display_name;
                    }
                }
            }
            if (!group.catalog_game_id.empty() && meta != nullptr && meta->ready()) {
                // Remember disc game code so later resolves hit without stem heuristics.
                (void)meta->upsert_alias(
                    game_meta_alias::kSerial,
                    to_lower(game_code),
                    "gamecube",
                    group.catalog_game_id);
            }
        }
    }

    std::unordered_set<std::string> seen_catalog;
    for (auto& [_, group] : by_code) {
        if (group.bytes == 0 || group.game_code.empty()) {
            continue;
        }
        SaveGameEntry entry{
            username,
            "gamecube",
            save_system_label("gamecube"),
            "gamecube:gci:" + group.game_code,
            group.display.empty() ? group.game_code : group.display,
            group.primary,
            group.bytes,
        };
        entry.catalog_game_id = group.catalog_game_id;
        if (!entry.catalog_game_id.empty()) {
            seen_catalog.insert(entry.catalog_game_id);
        }
        out.push_back(std::move(entry));
    }
    append_meta_system_entries(out, username, "gamecube", meta, seen_catalog);
}

std::optional<std::string> wii_title_low_to_game_code(std::string_view low_hex) {
    if (low_hex.size() != 8) {
        return std::nullopt;
    }
    std::string code;
    code.resize(4);
    for (std::size_t i = 0; i < 4; ++i) {
        const auto hi = low_hex[i * 2];
        const auto lo = low_hex[i * 2 + 1];
        auto nibble = [](char c) -> int {
            if (c >= '0' && c <= '9') {
                return c - '0';
            }
            if (c >= 'a' && c <= 'f') {
                return 10 + (c - 'a');
            }
            if (c >= 'A' && c <= 'F') {
                return 10 + (c - 'A');
            }
            return -1;
        };
        const int a = nibble(hi);
        const int b = nibble(lo);
        if (a < 0 || b < 0) {
            return std::nullopt;
        }
        code[i] = static_cast<char>((a << 4) | b);
        if (!std::isalnum(static_cast<unsigned char>(code[i]))) {
            return std::nullopt;
        }
    }
    return code;
}

void append_dolphin_wii_entries(
    std::vector<SaveGameEntry>& out,
    const std::string& username,
    const std::filesystem::path& user_dir,
    const SaveNameHints& hints,
    GameMetaStore* meta) {
    const auto title_root = user_dir / "saves" / "dolphin-emu" / "User" / "Wii" / "title";
    std::error_code ec;
    if (!std::filesystem::is_directory(title_root, ec)) {
        append_meta_system_entries(out, username, "wii", meta, {});
        return;
    }

    std::unordered_set<std::string> seen_catalog;
    for (const auto& high_entry : std::filesystem::directory_iterator(title_root, ec)) {
        if (!high_entry.is_directory(ec)) {
            continue;
        }
        const auto high = high_entry.path().filename().string();
        // Disc games are under 00010000; skip system titles (00000001, …).
        if (high != "00010000" && high != "00010001" && high != "00010004") {
            continue;
        }
        for (const auto& low_entry : std::filesystem::directory_iterator(high_entry.path(), ec)) {
            if (!low_entry.is_directory(ec)) {
                continue;
            }
            const auto low = low_entry.path().filename().string();
            const auto data_dir = low_entry.path() / "data";
            std::uint64_t bytes = 0;
            if (std::filesystem::is_directory(data_dir, ec)) {
                bytes = directory_bytes(data_dir);
            } else {
                bytes = directory_bytes(low_entry.path());
            }
            if (bytes == 0) {
                continue;
            }
            const auto game_code = wii_title_low_to_game_code(low).value_or(low);
            std::string display = game_code;
            std::string catalog_id;
            if (auto hit = resolve_dolphin_title("wii", game_code, game_code, hints, meta)) {
                display = hit->second;
            }
            if (meta != nullptr && meta->ready()) {
                if (auto row = meta->resolve(game_code, "wii")) {
                    catalog_id = row->game_id;
                    if (!row->display_name.empty()) {
                        display = row->display_name;
                    }
                } else if (auto row = meta->resolve(display, "wii")) {
                    catalog_id = row->game_id;
                }
            }
            SaveGameEntry entry{
                username,
                "wii",
                save_system_label("wii"),
                "wii:title:" + high + "/" + low,
                std::move(display),
                std::filesystem::is_directory(data_dir, ec) ? data_dir : low_entry.path(),
                bytes,
            };
            entry.catalog_game_id = catalog_id;
            if (!catalog_id.empty()) {
                seen_catalog.insert(catalog_id);
            }
            out.push_back(std::move(entry));
        }
    }
    append_meta_system_entries(out, username, "wii", meta, seen_catalog);
}

void append_meta_system_entries(
    std::vector<SaveGameEntry>& out,
    const std::string& username,
    std::string_view system_key,
    GameMetaStore* meta,
    const std::unordered_set<std::string>& skip_catalog_ids) {
    if (meta == nullptr || !meta->ready()) {
        return;
    }
    for (const auto& played : meta->list_user_games(username, system_key)) {
        if (skip_catalog_ids.contains(played.game_id)) {
            continue;
        }
        std::string display = played.game_id;
        if (const auto row = meta->find_by_id(played.game_id)) {
            display = row->display_name.empty() ? row->canonical_name : row->display_name;
            if (display.empty()) {
                display = played.game_id;
            }
        } else if (const auto row = meta->resolve(played.game_id, system_key)) {
            display = row->display_name;
        }
        std::string key;
        if (system_key == "ps2") {
            key = ps2_meta_game_key(played.game_id);
        } else if (system_key == "gamecube") {
            key = gamecube_meta_game_key(played.game_id);
        } else if (system_key == "wii") {
            key = wii_meta_game_key(played.game_id);
        } else {
            key = std::string(system_key) + ":id:" + played.game_id;
        }
        SaveGameEntry entry{
            username,
            std::string(system_key),
            save_system_label(system_key),
            std::move(key),
            std::move(display),
            {},
            0,
        };
        entry.catalog_game_id = played.game_id;
        out.push_back(std::move(entry));
    }
}

void append_file_entries(
    std::vector<SaveGameEntry>& out,
    const std::string& username,
    const std::filesystem::path& user_dir,
    const SaveNameHints& hints,
    GameMetaStore* meta) {
    const auto saves = user_dir / "saves";
    std::error_code ec;
    if (!std::filesystem::is_directory(saves, ec)) {
        return;
    }

    // Group by relative stem so .srm+.rtc+.sav collapse to one row.
    struct Group {
        std::filesystem::path primary;
        std::string relative_key;
        std::uint64_t bytes = 0;
        std::string system_key;
        std::string display;
    };
    std::unordered_map<std::string, Group> groups;

    for (const auto& entry : std::filesystem::recursive_directory_iterator(
             saves, std::filesystem::directory_options::skip_permission_denied, ec)) {
        if (!entry.is_regular_file(ec)) {
            continue;
        }
        auto ext = entry.path().extension().string();
        if (!ext.empty() && ext.front() == '.') {
            ext.erase(ext.begin());
        }
        const auto ext_l = to_lower(ext);
        if (!save_extension(ext_l)) {
            continue;
        }
        const auto rel = std::filesystem::relative(entry.path(), saves, ec);
        if (ec) {
            continue;
        }
        // Dolphin GC/Wii trees are handled by dedicated appenders (skip SRAM.raw etc.).
        {
            bool under_dolphin = false;
            for (const auto& part : rel) {
                const auto name = to_lower(part.string());
                if (name == "dolphin-emu" || name == "dolphin") {
                    under_dolphin = true;
                    break;
                }
            }
            if (under_dolphin) {
                continue;
            }
        }
        const auto group_key = to_lower(rel.parent_path().string()) + "|" + to_lower(rel.stem().string());
        auto labels = resolve_file_labels(rel, hints, meta);
        auto& group = groups[group_key];
        group.bytes += directory_bytes(entry.path());
        if (group.primary.empty() || ext_l == "srm" || ext_l == "sav" || ext_l == "dsv") {
            group.primary = entry.path();
            group.relative_key = rel.generic_string();
            group.system_key = normalize_browser_system_key(labels.first);
            group.display = labels.second;
        }
    }

    for (auto& [_, group] : groups) {
        if (group.bytes == 0 || group.relative_key.empty()) {
            continue;
        }
        SaveGameEntry entry{
            username,
            group.system_key,
            save_system_label(group.system_key),
            "file:" + group.relative_key,
            group.display,
            group.primary,
            group.bytes,
        };
        annotate_file_save_catalog(entry, group.primary.stem().string(), meta);
        out.push_back(std::move(entry));
    }
}

std::vector<SaveGameEntry> list_user_games(
    const std::filesystem::path& save_root,
    const std::string& username,
    const SaveNameHints& hints,
    GameMetaStore* meta) {
    const auto user_dir = save_root / username;
    std::vector<SaveGameEntry> out;
    append_switch_entries(out, username, user_dir, hints, meta);
    append_ps2_entries(out, username, user_dir, meta);
    append_dolphin_gamecube_entries(out, username, user_dir, hints, meta);
    append_dolphin_wii_entries(out, username, user_dir, hints, meta);
    append_file_entries(out, username, user_dir, hints, meta);
    std::sort(out.begin(), out.end(), [](const SaveGameEntry& a, const SaveGameEntry& b) {
        if (a.system_label != b.system_label) {
            return a.system_label < b.system_label;
        }
        return a.display_name < b.display_name;
    });
    return out;
}

SaveNameHints merge_save_name_hints(const SaveNameHints& base) {
    SaveNameHints merged = base;
    try {
        GameMetaStore meta;
        if (!meta.ready()) {
            return merged;
        }
        const auto from_meta = meta.save_name_hints();
        for (const auto& [key, value] : from_meta.by_stem) {
            merged.by_stem[key] = value;
        }
    } catch (...) {
        // Soft-fail: callers still have catalog/in-memory hints.
    }
    return merged;
}

void delete_switch_game_mirrors(
    const std::filesystem::path& user_dir,
    std::string_view title_or_leaf) {
    const auto leaf = std::string(title_or_leaf);
    const auto tid = looks_like_title_id(leaf) ? normalize_switch_title_id(leaf) : std::string{};

    // Catalog addon tree for stem-keyed saves.
    if (!tid.empty()) {
        // legacy title-id leaf — no addons dir by title id
    } else if (!leaf.empty()) {
        remove_path_best_effort(user_dir / "switch" / "addons" / leaf);
    }

    // Yuzu NAND title dirs (any case) — only when deleting a title-id leaf.
    const auto yuzu_save = user_dir / "yuzu" / "xdg-data" / "yuzu" / "nand" / "user" / "save";
    std::error_code ec;
    if (std::filesystem::is_directory(yuzu_save, ec) && !tid.empty()) {
        for (const auto& account : std::filesystem::directory_iterator(yuzu_save, ec)) {
            if (!account.is_directory(ec)) {
                continue;
            }
            for (const auto& user_hash : std::filesystem::directory_iterator(account.path(), ec)) {
                if (!user_hash.is_directory(ec)) {
                    continue;
                }
                for (const auto& title_dir : std::filesystem::directory_iterator(user_hash.path(), ec)) {
                    if (!title_dir.is_directory(ec)) {
                        continue;
                    }
                    if (normalize_switch_title_id(title_dir.path().filename().string()) == tid) {
                        remove_path_best_effort(title_dir.path());
                    }
                }
            }
        }
    }

    // Ryujinx BIS saves whose ExtraData title id matches — only for title-id deletes.
    // Stem deletes leave BIS alone (another stem may still use the same title id).
    const auto ryu_save = user_dir / "ryujinx" / "xdg-config" / "Ryujinx" / "bis" / "user" / "save";
    const auto ryu_meta = user_dir / "ryujinx" / "xdg-config" / "Ryujinx" / "bis" / "user" / "saveMeta";
    if (std::filesystem::is_directory(ryu_save, ec) && !tid.empty()) {
        std::vector<std::filesystem::path> doomed;
        for (const auto& save_id : std::filesystem::directory_iterator(ryu_save, ec)) {
            if (!save_id.is_directory(ec)) {
                continue;
            }
            bool match = false;
            for (const char* name : {"ExtraData0", "ExtraData1"}) {
                const auto ed = save_id.path() / name;
                if (!std::filesystem::is_regular_file(ed, ec)) {
                    continue;
                }
                if (title_id_from_extra_data(ed) == tid) {
                    match = true;
                    break;
                }
            }
            if (match) {
                doomed.push_back(save_id.path());
            }
        }
        for (const auto& path : doomed) {
            remove_path_best_effort(ryu_meta / path.filename());
            remove_path_best_effort(path);
        }
    }
}

void delete_file_game(
    const std::filesystem::path& user_dir,
    std::string_view relative_key) {
    const auto saves = user_dir / "saves";
    const auto primary = saves / std::filesystem::path(std::string(relative_key));
    const auto stem = primary.stem().string();
    const auto parent = primary.parent_path();
    std::error_code ec;
    if (std::filesystem::is_directory(parent, ec)) {
        for (const auto& entry : std::filesystem::directory_iterator(parent, ec)) {
            if (!entry.is_regular_file(ec)) {
                continue;
            }
            if (entry.path().stem() == stem) {
                remove_path_best_effort(entry.path());
            }
        }
    } else {
        remove_path_best_effort(primary);
    }

    // Matching RetroArch states (flat under states/).
    const auto states = user_dir / "states";
    if (std::filesystem::is_directory(states, ec)) {
        for (const auto& entry : std::filesystem::directory_iterator(states, ec)) {
            if (!entry.is_regular_file(ec)) {
                continue;
            }
            const auto name = entry.path().filename().string();
            if (name.rfind(stem, 0) == 0) {
                remove_path_best_effort(entry.path());
            }
        }
    }
}

} // namespace

std::string save_system_label(std::string_view system_key) {
    if (system_key == "switch") {
        return "Nintendo Switch";
    }
    if (system_key == "ps2") {
        return "PlayStation 2";
    }
    if (system_key == "ps1") {
        return "PlayStation";
    }
    if (system_key == "nds") {
        return "Nintendo DS";
    }
    if (system_key == "gba") {
        return "Game Boy Advance";
    }
    if (system_key == "gb" || system_key == "gbc" || system_key == "gb-gbc") {
        return "Game Boy / Color";
    }
    if (system_key == "snes") {
        return "SNES";
    }
    if (system_key == "nes") {
        return "NES";
    }
    if (system_key == "n64") {
        return "Nintendo 64";
    }
    if (system_key == "gamecube") {
        return "GameCube";
    }
    if (system_key == "wii") {
        return "Wii";
    }
    if (system_key == "3ds") {
        return "Nintendo 3DS";
    }
    if (system_key == "psp") {
        return "PSP";
    }
    if (system_key == "sega-8-16") {
        return "Sega 8/16-bit";
    }
    if (system_key == "other" || system_key.empty()) {
        return "Other";
    }
    return std::string(system_key);
}

std::string normalize_save_browser_system_key(std::string_view system_key) {
    if (system_key == "gb" || system_key == "gbc" || system_key == "gb-gbc") {
        return "gb-gbc";
    }
    return std::string(system_key);
}

std::vector<std::string> list_save_users(const std::filesystem::path& save_root) {
    std::vector<std::string> users;
    std::error_code ec;
    if (!std::filesystem::is_directory(save_root, ec)) {
        return users;
    }
    for (const auto& entry : std::filesystem::directory_iterator(save_root, ec)) {
        if (!entry.is_directory(ec)) {
            continue;
        }
        const auto name = entry.path().filename().string();
        if (is_reserved_user(name) || !valid_username(name)) {
            continue;
        }
        users.push_back(name);
    }
    std::sort(users.begin(), users.end());
    return users;
}

std::vector<SaveGameEntry> list_save_games(
    const std::filesystem::path& save_root,
    std::string_view username,
    std::string_view system_filter,
    const SaveNameHints& hints) {
    harvest_ryujinx_title_aliases(save_root);
    const auto merged_hints = merge_save_name_hints(hints);
    std::optional<GameMetaStore> meta_store;
    GameMetaStore* meta_ptr = nullptr;
    try {
        meta_store.emplace();
        if (meta_store->ready()) {
            meta_ptr = &*meta_store;
        }
    } catch (...) {
        meta_ptr = nullptr;
    }
    // Ensure live sessions appear under the user (PS2 and others) as user+game rows.
    if (meta_ptr != nullptr) {
        for (const auto& active : list_active_save_sessions(save_root)) {
            if (!active.username.empty() && !active.game_id.empty()) {
                (void)meta_ptr->record_user_game(
                    active.username, active.game_id, active.system_key);
            }
        }
    }
    std::vector<SaveGameEntry> out;
    const auto users = username.empty()
        ? list_save_users(save_root)
        : std::vector<std::string>{std::string(username)};
    for (const auto& user : users) {
        if (!username.empty() && user != username) {
            continue;
        }
        if (is_reserved_user(user) || !valid_username(user)) {
            continue;
        }
        auto games = list_user_games(save_root, user, merged_hints, meta_ptr);
        const auto want = normalize_browser_system_key(system_filter);
        for (auto& game : games) {
            game.system_key = normalize_browser_system_key(game.system_key);
            game.system_label = save_system_label(game.system_key);
            if (!want.empty() && game.system_key != want) {
                continue;
            }
            out.push_back(std::move(game));
        }
    }
    // Lock memcard disk reads only after a Users-tab pass actually allowed parsing.
    if (ps2_memcard_users_tab_opened()) {
        ps2_memcard_finish_initial_scan();
    }
    return out;
}

std::vector<std::string> list_save_systems(
    const std::filesystem::path& save_root,
    std::string_view username,
    const SaveNameHints& hints) {
    std::unordered_set<std::string> keys;
    for (const auto& game : list_save_games(save_root, username, {}, hints)) {
        keys.insert(game.system_key);
    }
    std::vector<std::string> out(keys.begin(), keys.end());
    std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) {
        return save_system_label(a) < save_system_label(b);
    });
    return out;
}

SaveProfile create_save_user(
    const std::filesystem::path& save_root,
    const std::string& username) {
    if (!valid_username(username) || is_reserved_user(username)) {
        throw std::runtime_error("invalid username");
    }
    if (std::filesystem::exists(save_root / username)) {
        throw std::runtime_error("user already exists");
    }
    auto profile = prepare_save_profile(save_root, username);
    ensure_default_credentials(profile.user_directory);
    return profile;
}

void delete_save_user(
    const std::filesystem::path& save_root,
    const std::string& username) {
    if (!valid_username(username) || is_reserved_user(username)) {
        throw std::runtime_error("invalid username");
    }
    const auto path = save_root / username;
    if (!std::filesystem::exists(path)) {
        throw std::runtime_error("user not found");
    }
    std::error_code ec;
    const auto removed = std::filesystem::remove_all(path, ec);
    if (ec || removed == 0) {
        throw std::runtime_error("failed to delete user: " + ec.message());
    }
    auto store = cadence::make_runtime_store();
    if (store->ensure_ready()) {
        (void)store->delete_user(username);
    }
}

std::size_t delete_save_system(
    const std::filesystem::path& save_root,
    const std::string& username,
    std::string_view system_key,
    const SaveNameHints& hints) {
    if (!valid_username(username) || is_reserved_user(username)) {
        throw std::runtime_error("invalid username");
    }
    if (system_key.empty()) {
        throw std::runtime_error("system required");
    }
    auto games = list_save_games(save_root, username, system_key, hints);
    for (const auto& game : games) {
        delete_save_game(save_root, username, game.game_key);
    }

    if (system_key == "switch") {
        const auto user_dir = save_root / username;
        remove_path_best_effort(user_dir / "switch" / "saves");
        remove_path_best_effort(
            user_dir / "ryujinx" / "xdg-config" / "Ryujinx" / "bis" / "user" / "save");
        remove_path_best_effort(
            user_dir / "ryujinx" / "xdg-config" / "Ryujinx" / "bis" / "user" / "saveMeta");
        // Recreate empty save dirs so the next prepare/sync path stays healthy.
        std::error_code ec;
        std::filesystem::create_directories(user_dir / "switch" / "saves", ec);
        std::filesystem::create_directories(
            user_dir / "ryujinx" / "xdg-config" / "Ryujinx" / "bis" / "user" / "save", ec);
    }
    if (system_key == "ps2") {
        try {
            GameMetaStore meta;
            if (meta.ready()) {
                (void)meta.remove_user_games_for_system(username, "ps2");
            }
        } catch (...) {
        }
        const auto user_dir = save_root / username;
        remove_path_best_effort(user_dir / "pcsx2" / "memcards");
        std::error_code ec;
        std::filesystem::create_directories(user_dir / "pcsx2" / "memcards", ec);
    }
    if (system_key == "gamecube" || system_key == "wii") {
        try {
            GameMetaStore meta;
            if (meta.ready()) {
                (void)meta.remove_user_games_for_system(username, system_key);
            }
        } catch (...) {
        }
        // Physical Dolphin files are removed per-game via delete_save_game; wiping the
        // whole User/GC or User/Wii tree would drop the other system's saves too.
    }
    return games.size();
}

void delete_save_game(
    const std::filesystem::path& save_root,
    const std::string& username,
    std::string_view game_key) {
    if (!valid_username(username) || is_reserved_user(username)) {
        throw std::runtime_error("invalid username");
    }
    const auto key = std::string(game_key);
    const auto user_dir = save_root / username;
    const auto colon = key.find(':');
    if (colon == std::string::npos) {
        throw std::runtime_error("invalid game key");
    }
    const auto kind = key.substr(0, colon);
    const auto rest = key.substr(colon + 1);
    if (rest.empty()) {
        throw std::runtime_error("invalid game key");
    }

    if (kind == "switch") {
        remove_path_best_effort(user_dir / "switch" / "saves" / rest);
        delete_switch_game_mirrors(user_dir, rest);
        return;
    }
    if (kind == "ps2") {
        // Meta-backed association: ps2:id:<catalog_game_id>
        if (rest.rfind("id:", 0) == 0) {
            const auto game_id = rest.substr(3);
            try {
                GameMetaStore meta;
                if (meta.ready()) {
                    (void)meta.remove_user_game(username, game_id);
                }
            } catch (...) {
            }
            return;
        }
        // Legacy memcard filename cleanup (no longer listed, but still removable).
        const auto cards = user_dir / "pcsx2" / "memcards";
        remove_path_best_effort(cards / rest);
        std::error_code ec;
        if (std::filesystem::is_directory(cards, ec)) {
            const auto prefix = rest + ".";
            for (const auto& entry : std::filesystem::directory_iterator(cards, ec)) {
                if (!entry.is_regular_file(ec)) {
                    continue;
                }
                const auto name = entry.path().filename().string();
                if (name.rfind(prefix, 0) == 0) {
                    remove_path_best_effort(entry.path());
                }
            }
        }
        return;
    }
    if (kind == "gamecube") {
        if (rest.rfind("id:", 0) == 0) {
            try {
                GameMetaStore meta;
                if (meta.ready()) {
                    (void)meta.remove_user_game(username, rest.substr(3));
                }
            } catch (...) {
            }
            return;
        }
        if (rest.rfind("gci:", 0) == 0) {
            const auto game_code = rest.substr(4);
            const auto gc_root = user_dir / "saves" / "dolphin-emu" / "User" / "GC";
            std::error_code ec;
            if (!std::filesystem::is_directory(gc_root, ec)) {
                return;
            }
            std::vector<std::filesystem::path> doomed;
            for (const auto& entry : std::filesystem::recursive_directory_iterator(
                     gc_root, std::filesystem::directory_options::skip_permission_denied, ec)) {
                if (!entry.is_regular_file(ec)) {
                    continue;
                }
                if (to_lower(entry.path().extension().string()) != ".gci") {
                    continue;
                }
                std::string code;
                std::string internal;
                if (!read_gci_identity(entry.path(), code, internal)) {
                    continue;
                }
                if (to_lower(code) == to_lower(game_code)) {
                    doomed.push_back(entry.path());
                }
            }
            for (const auto& path : doomed) {
                remove_path_best_effort(path);
            }
            return;
        }
        throw std::runtime_error("invalid gamecube game key");
    }
    if (kind == "wii") {
        if (rest.rfind("id:", 0) == 0) {
            try {
                GameMetaStore meta;
                if (meta.ready()) {
                    (void)meta.remove_user_game(username, rest.substr(3));
                }
            } catch (...) {
            }
            return;
        }
        if (rest.rfind("title:", 0) == 0) {
            const auto rel = rest.substr(6); // high/low
            remove_path_best_effort(
                user_dir / "saves" / "dolphin-emu" / "User" / "Wii" / "title" / rel);
            return;
        }
        throw std::runtime_error("invalid wii game key");
    }
    if (kind == "file") {
        delete_file_game(user_dir, rest);
        return;
    }
    throw std::runtime_error("unknown game key kind");
}

bool user_has_mismatched_save_for_game(
    const std::filesystem::path& save_root,
    std::string_view username,
    std::string_view game_id) {
    if (save_root.empty() || username.empty() || game_id.empty()) {
        return false;
    }
    try {
        for (const auto& game : list_save_games(save_root, username)) {
            if (game.save_stem_mismatch && game.catalog_game_id == game_id) {
                return true;
            }
        }
    } catch (...) {
        return false;
    }
    return false;
}

} // namespace archstreamer
