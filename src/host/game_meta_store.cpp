#include "host/game_meta_store.hpp"

#include "common/game_identity.hpp"
#include "common/platform/paths.hpp"
#include "common/serialization.hpp"
#include "common/sha256.hpp"
#include "host/game_catalog_scanner.hpp"
#include "host/switch_save_share.hpp"

#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

namespace archstreamer {

std::filesystem::path guess_rom_meta_json(const std::filesystem::path& content_path) {
    auto beside = content_path;
    beside.replace_extension(".json");
    std::error_code ec;
    if (std::filesystem::is_regular_file(beside, ec)) {
        return beside;
    }
    std::vector<std::filesystem::path> parts;
    for (const auto& part : content_path) {
        parts.push_back(part);
    }
    for (std::size_t i = 0; i < parts.size(); ++i) {
        const auto token = [&] {
            auto s = parts[i].string();
            for (char& ch : s) {
                ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            }
            return s;
        }();
        if (token != "games") {
            continue;
        }
        std::filesystem::path rebuilt;
        for (std::size_t j = 0; j < parts.size(); ++j) {
            rebuilt /= (j == i) ? "Meta" : parts[j];
        }
        rebuilt.replace_extension(".json");
        if (std::filesystem::is_regular_file(rebuilt, ec)) {
            return rebuilt;
        }
        break;
    }
    return {};
}

namespace {

std::int64_t meta_created_unix_seconds(const std::filesystem::path& content_path) {
    const auto meta_json = guess_rom_meta_json(content_path);
    if (meta_json.empty()) {
        return 0;
    }
    std::error_code ec;
    const auto meta_dir = meta_json.parent_path();
    if (std::filesystem::is_directory(meta_dir, ec)) {
        if (const auto dir_t = file_birth_or_mtime_unix_seconds(meta_dir); dir_t > 0) {
            return dir_t;
        }
    }
    if (std::filesystem::is_regular_file(meta_json, ec)) {
        return file_birth_or_mtime_unix_seconds(meta_json);
    }
    return 0;
}

std::string to_lower_copy(std::string value) {
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
    const auto lower = to_lower_copy(std::string(value));
    return lower.rfind("0100", 0) == 0;
}

std::int64_t now_epoch_seconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string normalize_alias_value(std::string_view kind, std::string_view value) {
    if (value.empty()) {
        return {};
    }
    if (kind == game_meta_alias::kTitleId) {
        return normalize_switch_title_id(value);
    }
    if (kind == game_meta_alias::kCatalogId) {
        return std::string(value);
    }
    // Fold accents so "Pokémon Sword" matches catalog "Pokemon Sword".
    return to_lower_copy(fold_common_latin_accents(std::string(value)));
}

std::string strip_trailing_version_label(std::string name) {
    // "Pokemon Sword 1.3.2" → "Pokemon Sword"
    while (!name.empty() && (std::isdigit(static_cast<unsigned char>(name.back())) || name.back() == '.'
                             || name.back() == ' ')) {
        if (name.back() == ' ') {
            name.pop_back();
            break;
        }
        name.pop_back();
    }
    while (!name.empty() && name.back() == ' ') {
        name.pop_back();
    }
    return name;
}

GameMetaRecord row_from_stmt(sqlite3_stmt* stmt) {
    GameMetaRecord row;
    row.game_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    row.system_key = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
    row.system_name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
    row.display_name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
    row.canonical_name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
    row.core_name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
    row.asset_key = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
    row.identity_key = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 7));
    row.version = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 8));
    row.language = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 9));
    row.region = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 10));
    row.content_stem = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 11));
    row.created_at = sqlite3_column_int64(stmt, 12);
    row.updated_at = sqlite3_column_int64(stmt, 13);
    row.source = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 14));
    if (sqlite3_column_count(stmt) > 15 && sqlite3_column_text(stmt, 15) != nullptr) {
        row.content_path = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 15));
    }
    return row;
}

void read_optional_meta_aliases(
    const std::filesystem::path& content_path,
    GameMetaRecord& row,
    std::vector<std::pair<std::string, std::string>>& extra_aliases) {
    try {
        const auto metadata_path = guess_rom_meta_json(content_path);
        if (metadata_path.empty()) {
            return;
        }
        std::ifstream file(metadata_path);
        if (!file) {
            return;
        }
        const auto metadata = nlohmann::json::parse(file);
        if (!metadata.is_object()) {
            return;
        }
        row.source = std::string(game_meta_source::kMetaJson);
        if (metadata.contains("title_id")) {
            const auto tid = metadata.at("title_id").get<std::string>();
            if (!tid.empty()) {
                extra_aliases.emplace_back(std::string(game_meta_alias::kTitleId), tid);
            }
        }
        if (metadata.contains("serial")) {
            const auto serial = metadata.at("serial").get<std::string>();
            if (!serial.empty()) {
                extra_aliases.emplace_back(std::string(game_meta_alias::kSerial), serial);
            }
        }
    } catch (...) {
        // Soft-fail.
    }
}

} // namespace

std::filesystem::path GameMetaStore::default_path() {
    const auto home = user_home_directory();
    if (!home.empty()) {
        return std::filesystem::path(home) / ".local" / "share" / "archstreamer" / "meta"
            / "games.sqlite";
    }
    return std::filesystem::current_path() / "archstreamer-meta" / "games.sqlite";
}

GameMetaStore::GameMetaStore(std::filesystem::path db_path)
    : db_path_(std::move(db_path)) {
    (void)open();
}

GameMetaStore::~GameMetaStore() {
    if (db_ != nullptr) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

GameMetaStore::GameMetaStore(GameMetaStore&& other) noexcept
    : db_path_(std::move(other.db_path_))
    , db_(other.db_) {
    other.db_ = nullptr;
}

GameMetaStore& GameMetaStore::operator=(GameMetaStore&& other) noexcept {
    if (this != &other) {
        if (db_ != nullptr) {
            sqlite3_close(db_);
        }
        db_path_ = std::move(other.db_path_);
        db_ = other.db_;
        other.db_ = nullptr;
    }
    return *this;
}

bool GameMetaStore::exec(const char* sql) {
    char* err = nullptr;
    const int rc = sqlite3_exec(db_, sql, nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        std::cerr << "game_meta: SQL error: " << (err ? err : sqlite3_errmsg(db_)) << '\n';
        sqlite3_free(err);
        return false;
    }
    return true;
}

bool GameMetaStore::exec_quiet(const char* sql) {
    char* err = nullptr;
    const int rc = sqlite3_exec(db_, sql, nullptr, nullptr, &err);
    sqlite3_free(err);
    return rc == SQLITE_OK;
}

bool GameMetaStore::ensure_schema() {
    if (!exec(
            "CREATE TABLE IF NOT EXISTS game_meta ("
            "  game_id TEXT PRIMARY KEY NOT NULL,"
            "  system_key TEXT NOT NULL DEFAULT '',"
            "  system_name TEXT NOT NULL DEFAULT '',"
            "  display_name TEXT NOT NULL DEFAULT '',"
            "  canonical_name TEXT NOT NULL DEFAULT '',"
            "  core_name TEXT NOT NULL DEFAULT '',"
            "  asset_key TEXT NOT NULL DEFAULT '',"
            "  identity_key TEXT NOT NULL DEFAULT '',"
            "  version TEXT NOT NULL DEFAULT '',"
            "  language TEXT NOT NULL DEFAULT '',"
            "  region TEXT NOT NULL DEFAULT '',"
            "  content_stem TEXT NOT NULL DEFAULT '',"
            "  created_at INTEGER NOT NULL DEFAULT 0,"
            "  updated_at INTEGER NOT NULL DEFAULT 0,"
            "  source TEXT NOT NULL DEFAULT 'catalog'"
            ");")) {
        return false;
    }
    if (!exec(
            "CREATE TABLE IF NOT EXISTS game_aliases ("
            "  alias_kind TEXT NOT NULL,"
            "  alias_value TEXT NOT NULL,"
            "  system_key TEXT NOT NULL DEFAULT '',"
            "  game_id TEXT NOT NULL,"
            "  PRIMARY KEY (alias_kind, alias_value, system_key)"
            ");")) {
        return false;
    }
    (void)exec_quiet(
        "CREATE INDEX IF NOT EXISTS idx_game_aliases_value ON game_aliases(alias_value);");
    (void)exec_quiet(
        "CREATE INDEX IF NOT EXISTS idx_game_aliases_game_id ON game_aliases(game_id);");
    if (!exec(
            "CREATE TABLE IF NOT EXISTS user_games ("
            "  username TEXT NOT NULL,"
            "  game_id TEXT NOT NULL,"
            "  system_key TEXT NOT NULL DEFAULT '',"
            "  last_played_at INTEGER NOT NULL DEFAULT 0,"
            "  PRIMARY KEY (username, game_id)"
            ");")) {
        return false;
    }
    if (!exec(
            "CREATE TABLE IF NOT EXISTS user_game_blocks ("
            "  username TEXT NOT NULL,"
            "  game_id TEXT NOT NULL,"
            "  system_key TEXT NOT NULL DEFAULT '',"
            "  created_at INTEGER NOT NULL DEFAULT 0,"
            "  PRIMARY KEY (username, game_id)"
            ");")) {
        return false;
    }
    if (!exec(
            "CREATE TABLE IF NOT EXISTS game_play_modes ("
            "  game_id TEXT PRIMARY KEY NOT NULL,"
            "  supports_single INTEGER NOT NULL DEFAULT 1,"
            "  supports_multi INTEGER NOT NULL DEFAULT 1,"
            "  min_players INTEGER NOT NULL DEFAULT 1,"
            "  max_players INTEGER NOT NULL DEFAULT 2,"
            "  updated_at INTEGER NOT NULL DEFAULT 0,"
            "  source TEXT NOT NULL DEFAULT 'catalog'"
            ");")) {
        return false;
    }
    (void)exec_quiet(
        "ALTER TABLE game_meta ADD COLUMN content_path TEXT NOT NULL DEFAULT '';");
    (void)exec_quiet(
        "ALTER TABLE game_meta ADD COLUMN created_at INTEGER NOT NULL DEFAULT 0;");
    (void)exec_quiet(
        "CREATE INDEX IF NOT EXISTS idx_game_meta_content_path ON game_meta(content_path);");
    (void)exec_quiet(
        "CREATE INDEX IF NOT EXISTS idx_game_meta_content_stem ON game_meta(content_stem);");
    (void)exec_quiet(
        "CREATE INDEX IF NOT EXISTS idx_user_games_user_system"
        " ON user_games(username, system_key);");
    (void)exec_quiet(
        "CREATE INDEX IF NOT EXISTS idx_user_game_blocks_user"
        " ON user_game_blocks(username);");
    if (!exec(
            "CREATE TABLE IF NOT EXISTS catalog_offerings ("
            "  game_id TEXT PRIMARY KEY NOT NULL,"
            "  identity_key TEXT NOT NULL DEFAULT '',"
            "  asset_key TEXT NOT NULL DEFAULT '',"
            "  display_name TEXT NOT NULL DEFAULT '',"
            "  system_name TEXT NOT NULL DEFAULT '',"
            "  system_key TEXT NOT NULL DEFAULT '',"
            "  core_name TEXT NOT NULL DEFAULT '',"
            "  canonical_name TEXT NOT NULL DEFAULT '',"
            "  version TEXT NOT NULL DEFAULT '',"
            "  language TEXT NOT NULL DEFAULT '',"
            "  region TEXT NOT NULL DEFAULT '',"
            "  supports_single INTEGER NOT NULL DEFAULT 1,"
            "  supports_multi INTEGER NOT NULL DEFAULT 1,"
            "  min_players INTEGER NOT NULL DEFAULT 1,"
            "  max_players INTEGER NOT NULL DEFAULT 2,"
            "  updated_at INTEGER NOT NULL DEFAULT 0,"
            "  playlist_discs_json TEXT NOT NULL DEFAULT '[]'"
            ");")) {
        return false;
    }
    if (!exec(
            "CREATE TABLE IF NOT EXISTS catalog_offerings_state ("
            "  id INTEGER PRIMARY KEY CHECK (id = 1),"
            "  content_hash TEXT NOT NULL DEFAULT '',"
            "  revision INTEGER NOT NULL DEFAULT 0,"
            "  game_count INTEGER NOT NULL DEFAULT 0,"
            "  rebuilt_at INTEGER NOT NULL DEFAULT 0"
            ");")) {
        return false;
    }
    return true;
}

bool GameMetaStore::open() {
    std::error_code ec;
    std::filesystem::create_directories(db_path_.parent_path(), ec);
    if (sqlite3_open(db_path_.string().c_str(), &db_) != SQLITE_OK) {
        std::cerr << "game_meta: open failed: "
                  << (db_ ? sqlite3_errmsg(db_) : "unknown") << '\n';
        if (db_ != nullptr) {
            sqlite3_close(db_);
            db_ = nullptr;
        }
        return false;
    }
    if (!exec("PRAGMA journal_mode=WAL;")) {
        sqlite3_close(db_);
        db_ = nullptr;
        return false;
    }
    if (!exec("PRAGMA synchronous=NORMAL;")) {
        sqlite3_close(db_);
        db_ = nullptr;
        return false;
    }
    if (!ensure_schema()) {
        sqlite3_close(db_);
        db_ = nullptr;
        return false;
    }
    return true;
}

bool GameMetaStore::upsert(const GameMetaRecord& row) {
    if (db_ == nullptr || row.game_id.empty()) {
        return false;
    }
    constexpr const char* kSql =
        "INSERT INTO game_meta ("
        "  game_id, system_key, system_name, display_name, canonical_name, core_name,"
        "  asset_key, identity_key, version, language, region, content_stem,"
        "  created_at, updated_at, source, content_path"
        ") VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)"
        " ON CONFLICT(game_id) DO UPDATE SET"
        "  system_key=excluded.system_key,"
        "  system_name=excluded.system_name,"
        "  display_name=excluded.display_name,"
        "  canonical_name=excluded.canonical_name,"
        "  core_name=excluded.core_name,"
        "  asset_key=excluded.asset_key,"
        "  identity_key=excluded.identity_key,"
        "  version=excluded.version,"
        "  language=excluded.language,"
        "  region=excluded.region,"
        "  content_stem=excluded.content_stem,"
        "  created_at=CASE"
        "    WHEN game_meta.created_at > 0 THEN game_meta.created_at"
        "    ELSE excluded.created_at END,"
        "  updated_at=excluded.updated_at,"
        "  source=excluded.source,"
        "  content_path=excluded.content_path;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, kSql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    const auto updated =
        row.updated_at > 0 ? row.updated_at : now_epoch_seconds();
    const auto created =
        row.created_at > 0 ? row.created_at : updated;
    sqlite3_bind_text(stmt, 1, row.game_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, row.system_key.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, row.system_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, row.display_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, row.canonical_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, row.core_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 7, row.asset_key.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 8, row.identity_key.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 9, row.version.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 10, row.language.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 11, row.region.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 12, row.content_stem.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 13, created);
    sqlite3_bind_int64(stmt, 14, updated);
    sqlite3_bind_text(stmt, 15, row.source.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 16, row.content_path.c_str(), -1, SQLITE_TRANSIENT);
    const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

bool GameMetaStore::upsert_alias(
    std::string_view kind,
    std::string_view value,
    std::string_view system_key,
    std::string_view game_id) {
    if (db_ == nullptr || kind.empty() || game_id.empty()) {
        return false;
    }
    const auto normalized = normalize_alias_value(kind, value);
    if (normalized.empty()) {
        return false;
    }
    constexpr const char* kSql =
        "INSERT INTO game_aliases (alias_kind, alias_value, system_key, game_id)"
        " VALUES (?,?,?,?)"
        " ON CONFLICT(alias_kind, alias_value, system_key) DO UPDATE SET"
        "  game_id=excluded.game_id;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, kSql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    const auto kind_s = std::string(kind);
    const auto system_s = std::string(system_key);
    const auto game_s = std::string(game_id);
    sqlite3_bind_text(stmt, 1, kind_s.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, normalized.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, system_s.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, game_s.c_str(), -1, SQLITE_TRANSIENT);
    const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

std::optional<GameMetaRecord> GameMetaStore::load_by_id_stmt(std::string_view game_id) const {
    if (db_ == nullptr || game_id.empty()) {
        return std::nullopt;
    }
    constexpr const char* kSql =
        "SELECT "
        "game_id, system_key, system_name, display_name, canonical_name, core_name,"
        " asset_key, identity_key, version, language, region, content_stem,"
        " created_at, updated_at, source, content_path"
        " FROM game_meta WHERE game_id=? LIMIT 1;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, kSql, -1, &stmt, nullptr) != SQLITE_OK) {
        return std::nullopt;
    }
    const auto id = std::string(game_id);
    sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
    std::optional<GameMetaRecord> out;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        out = row_from_stmt(stmt);
    }
    sqlite3_finalize(stmt);
    return out;
}

std::optional<GameMetaRecord> GameMetaStore::find_by_id(std::string_view game_id) const {
    return load_by_id_stmt(game_id);
}

std::optional<std::string> GameMetaStore::lookup_alias_game_id(
    std::string_view kind,
    std::string_view value,
    std::string_view system_key) const {
    if (db_ == nullptr) {
        return std::nullopt;
    }
    const auto normalized = normalize_alias_value(kind, value);
    if (normalized.empty()) {
        return std::nullopt;
    }
    constexpr const char* kSql =
        "SELECT game_id FROM game_aliases"
        " WHERE alias_kind=? AND alias_value=? AND system_key=? LIMIT 1;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, kSql, -1, &stmt, nullptr) != SQLITE_OK) {
        return std::nullopt;
    }
    const auto kind_s = std::string(kind);
    const auto system_s = std::string(system_key);
    sqlite3_bind_text(stmt, 1, kind_s.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, normalized.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, system_s.c_str(), -1, SQLITE_TRANSIENT);
    std::optional<std::string> out;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        out = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    }
    sqlite3_finalize(stmt);
    return out;
}

std::optional<GameMetaRecord> GameMetaStore::resolve(
    std::string_view key,
    std::string_view system_key) const {
    if (key.empty()) {
        return std::nullopt;
    }
    if (auto by_id = find_by_id(key)) {
        return by_id;
    }

    auto try_kinds = [&](std::string_view scope) -> std::optional<GameMetaRecord> {
        static constexpr std::string_view kKinds[] = {
            game_meta_alias::kCatalogId,
            game_meta_alias::kTitleId,
            game_meta_alias::kSerial,
            game_meta_alias::kPs2Product,
            game_meta_alias::kContentStem,
            game_meta_alias::kCanonical,
            game_meta_alias::kDisplayName,
        };
        for (const auto kind : kKinds) {
            if (const auto game_id = lookup_alias_game_id(kind, key, scope)) {
                if (auto row = find_by_id(*game_id)) {
                    return row;
                }
            }
        }
        // Also match any kind by value alone within the scope.
        if (db_ == nullptr) {
            return std::nullopt;
        }
        constexpr const char* kAny =
            "SELECT game_id FROM game_aliases WHERE alias_value=? AND system_key=? LIMIT 1;";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, kAny, -1, &stmt, nullptr) != SQLITE_OK) {
            return std::nullopt;
        }
        const auto value = to_lower_copy(fold_common_latin_accents(std::string(key)));
        const auto title = normalize_switch_title_id(key);
        const auto scope_s = std::string(scope);
        // Prefer exact lowered key; title_id normalized is tried via kinds above.
        sqlite3_bind_text(stmt, 1, value.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, scope_s.c_str(), -1, SQLITE_TRANSIENT);
        std::optional<std::string> game_id;
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            game_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        }
        sqlite3_finalize(stmt);
        if (!game_id && !title.empty() && title != value) {
            if (sqlite3_prepare_v2(db_, kAny, -1, &stmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_text(stmt, 1, title.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 2, scope_s.c_str(), -1, SQLITE_TRANSIENT);
                if (sqlite3_step(stmt) == SQLITE_ROW) {
                    game_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
                }
                sqlite3_finalize(stmt);
            }
        }
        if (game_id) {
            return find_by_id(*game_id);
        }
        return std::nullopt;
    };

    // Empty / fallback scope: alias value in any system (not only system_key='').
    auto try_kinds_any_system = [&]() -> std::optional<GameMetaRecord> {
        if (db_ == nullptr) {
            return std::nullopt;
        }
        static constexpr std::string_view kKinds[] = {
            game_meta_alias::kCatalogId,
            game_meta_alias::kTitleId,
            game_meta_alias::kSerial,
            game_meta_alias::kPs2Product,
            game_meta_alias::kContentStem,
            game_meta_alias::kCanonical,
            game_meta_alias::kDisplayName,
        };
        const auto value = to_lower_copy(fold_common_latin_accents(std::string(key)));
        const auto title = normalize_switch_title_id(key);
        auto lookup_value = [&](std::string_view kind, const std::string& needle)
            -> std::optional<GameMetaRecord> {
            if (needle.empty()) {
                return std::nullopt;
            }
            // Prefer rows whose system_key is non-empty (real catalog aliases).
            constexpr const char* kSql =
                "SELECT game_id FROM game_aliases"
                " WHERE alias_kind=? AND alias_value=?"
                " ORDER BY CASE WHEN system_key='' THEN 1 ELSE 0 END"
                " LIMIT 1;";
            sqlite3_stmt* stmt = nullptr;
            if (sqlite3_prepare_v2(db_, kSql, -1, &stmt, nullptr) != SQLITE_OK) {
                return std::nullopt;
            }
            const auto kind_s = std::string(kind);
            sqlite3_bind_text(stmt, 1, kind_s.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, needle.c_str(), -1, SQLITE_TRANSIENT);
            std::optional<std::string> game_id;
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                game_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            }
            sqlite3_finalize(stmt);
            if (!game_id) {
                return std::nullopt;
            }
            return find_by_id(*game_id);
        };
        for (const auto kind : kKinds) {
            const auto normalized = normalize_alias_value(kind, key);
            if (auto hit = lookup_value(kind, normalized)) {
                return hit;
            }
            if (!title.empty() && title != normalized) {
                if (auto hit = lookup_value(kind, title)) {
                    return hit;
                }
            }
        }
        constexpr const char* kAny =
            "SELECT game_id FROM game_aliases WHERE alias_value=?"
            " ORDER BY CASE WHEN system_key='' THEN 1 ELSE 0 END LIMIT 1;";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, kAny, -1, &stmt, nullptr) != SQLITE_OK) {
            return std::nullopt;
        }
        sqlite3_bind_text(stmt, 1, value.c_str(), -1, SQLITE_TRANSIENT);
        std::optional<std::string> game_id;
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            game_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        }
        sqlite3_finalize(stmt);
        if (!game_id && !title.empty() && title != value) {
            if (sqlite3_prepare_v2(db_, kAny, -1, &stmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_text(stmt, 1, title.c_str(), -1, SQLITE_TRANSIENT);
                if (sqlite3_step(stmt) == SQLITE_ROW) {
                    game_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
                }
                sqlite3_finalize(stmt);
            }
        }
        if (game_id) {
            return find_by_id(*game_id);
        }
        return std::nullopt;
    };

    // Users-tab / save browser folds gb+gbc into "gb-gbc"; aliases stay on gb or gbc.
    auto try_system_scopes = [&](std::string_view scope) -> std::optional<GameMetaRecord> {
        if (scope == "gb-gbc") {
            static constexpr std::string_view kGbScopes[] = {"gb", "gbc", "gb-gbc"};
            for (const auto candidate : kGbScopes) {
                if (auto hit = try_kinds(candidate)) {
                    return hit;
                }
            }
            return std::nullopt;
        }
        return try_kinds(scope);
    };

    if (!system_key.empty()) {
        if (auto scoped = try_system_scopes(system_key)) {
            return scoped;
        }
    }
    // Global fallback: alias value in any system. Empty system_key used to only hit
    // aliases stored with system_key='' (almost never content_stem / display_name).
    if (auto any = try_kinds_any_system()) {
        return any;
    }
    return try_kinds({});
}

void GameMetaStore::sync_from_catalog(const GameCatalog& catalog) {
    if (db_ == nullptr) {
        return;
    }
    (void)exec("BEGIN;");
    for (const auto& game : catalog.list().games) {
        if (game.id.empty()) {
            continue;
        }
        GameInfo info = game;
        std::filesystem::path content_path;
        if (const auto hosted = catalog.find_hosted(game.id); hosted.has_value()) {
            content_path = hosted->get().content_path;
        }
        if (content_path.empty()) {
            continue;
        }
        bind_scanned_game(info, content_path);
    }
    (void)exec("COMMIT;");
}

std::optional<GameMetaRecord> GameMetaStore::find_by_content_path(
    std::string_view content_path) const {
    if (db_ == nullptr || content_path.empty()) {
        return std::nullopt;
    }
    constexpr const char* kSql =
        "SELECT game_id, system_key, system_name, display_name, canonical_name, core_name,"
        " asset_key, identity_key, version, language, region, content_stem,"
        " created_at, updated_at, source, content_path"
        " FROM game_meta WHERE content_path=? LIMIT 1;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, kSql, -1, &stmt, nullptr) != SQLITE_OK) {
        return std::nullopt;
    }
    const auto path = std::string(content_path);
    sqlite3_bind_text(stmt, 1, path.c_str(), -1, SQLITE_TRANSIENT);
    std::optional<GameMetaRecord> out;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        out = row_from_stmt(stmt);
    }
    sqlite3_finalize(stmt);
    return out;
}

namespace {

void apply_record_identity_to_game_info(const GameMetaRecord& row, GameInfo& info) {
    info.id = row.game_id;
    info.identity_key = row.identity_key;
    info.asset_key = row.asset_key;
    info.display_name = row.display_name;
    info.system_name = row.system_name.empty() ? info.system_name : row.system_name;
    info.system_key = row.system_key.empty() ? info.system_key : row.system_key;
    info.core_name = row.core_name.empty() ? info.core_name : row.core_name;
    info.canonical_name = row.canonical_name;
    info.version = row.version;
    info.language = row.language;
    info.region = row.region;
}

} // namespace

void GameMetaStore::bind_scanned_game(
    GameInfo& info,
    const std::filesystem::path& content_path) {
    if (db_ == nullptr || content_path.empty() || info.id.empty()) {
        return;
    }
    const auto path_s = content_path.lexically_normal().string();
    const auto stem = to_lower_copy(content_path.stem().string());
    const auto provisional_id = info.id;

    auto find_by_content_stem_column = [&](std::string_view needle)
        -> std::optional<GameMetaRecord> {
        if (needle.empty()) {
            return std::nullopt;
        }
        constexpr const char* kSql =
            "SELECT game_id, system_key, system_name, display_name, canonical_name, core_name,"
            " asset_key, identity_key, version, language, region, content_stem,"
            " created_at, updated_at, source, content_path"
            " FROM game_meta WHERE content_stem=? AND (system_key=? OR ?='')"
            " ORDER BY"
            " CASE WHEN source='meta_json' THEN 0 ELSE 1 END,"
            " CASE WHEN system_key=? THEN 0 ELSE 1 END"
            " LIMIT 1;";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, kSql, -1, &stmt, nullptr) != SQLITE_OK) {
            return std::nullopt;
        }
        const auto needle_s = std::string(needle);
        sqlite3_bind_text(stmt, 1, needle_s.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, info.system_key.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, info.system_key.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 4, info.system_key.c_str(), -1, SQLITE_TRANSIENT);
        std::optional<GameMetaRecord> out;
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            out = row_from_stmt(stmt);
        }
        sqlite3_finalize(stmt);
        return out;
    };

    auto fold_stale_into = [&](std::string_view stale_id, const GameMetaRecord& survivor) {
        if (stale_id.empty() || stale_id == survivor.game_id) {
            return;
        }
        if (auto stale = find_by_id(stale_id); stale && stale->game_id != survivor.game_id) {
            // Different builds of the same title are intentional catalog rows.
            if (!same_catalog_version(stale->version, survivor.version)) {
                return;
            }
            (void)migrate_user_games_game_id(stale_id, survivor.game_id);
            (void)migrate_play_modes_game_id(stale_id, survivor.game_id);
            (void)reassign_aliases_game_id(stale_id, survivor.game_id);
            (void)delete_game(stale_id);
        }
        (void)upsert_alias(game_meta_alias::kCatalogId, stale_id, {}, survivor.game_id);
        (void)upsert_alias(
            game_meta_alias::kCatalogId, stale_id, survivor.system_key, survivor.game_id);
    };

    // Prefer durable identity over path-derived stem matches so edited rows win
    // over resurrected catalog clones that still own the raw ROM stem alias.
    // Never merge across system_key — same stem on GBA vs NDS are different games.
    auto same_system = [&](const GameMetaRecord& row) {
        return info.system_key.empty() || row.system_key.empty()
            || row.system_key == info.system_key;
    };
    auto accept_existing = [&](std::optional<GameMetaRecord>& row, bool require_same_version) {
        if (!row) {
            return;
        }
        if (!same_system(*row)) {
            row = std::nullopt;
            return;
        }
        if (require_same_version && !same_catalog_version(row->version, info.version)) {
            row = std::nullopt;
        }
    };

    std::optional<GameMetaRecord> existing = find_by_content_path(path_s);
    if (!existing) {
        existing = find_by_id(provisional_id);
        accept_existing(existing, /*require_same_version=*/true);
    }
    if (!existing) {
        // An edit that changed identity deletes the old primary but keeps it as a
        // catalog_id alias. Follow that alias or the scan re-seeds the pre-edit row.
        for (const auto& scope : {info.system_key, std::string{}}) {
            if (const auto game_id =
                    lookup_alias_game_id(game_meta_alias::kCatalogId, provisional_id, scope)) {
                if (*game_id != provisional_id) {
                    existing = find_by_id(*game_id);
                }
            }
            // catalog_id aliases intentionally cross version (edit history); keep them.
            accept_existing(existing, /*require_same_version=*/false);
            if (existing) {
                break;
            }
        }
    }
    if (!existing && !stem.empty() && !info.system_key.empty()) {
        // Scoped resolve only — global content_stem aliases must not collapse
        // "Final Fantasy III" (GBA) into "Final Fantasy III" (NDS).
        if (const auto game_id = lookup_alias_game_id(
                game_meta_alias::kContentStem, stem, info.system_key)) {
            existing = find_by_id(*game_id);
        }
        if (!existing) {
            if (const auto game_id = lookup_alias_game_id(
                    game_meta_alias::kCanonical, stem, info.system_key)) {
                existing = find_by_id(*game_id);
            }
        }
        if (!existing) {
            if (const auto game_id = lookup_alias_game_id(
                    game_meta_alias::kDisplayName, stem, info.system_key)) {
                existing = find_by_id(*game_id);
            }
        }
        // Bare name/stem aliases are shared across builds — only accept same version.
        accept_existing(existing, /*require_same_version=*/true);
    }
    if (!existing) {
        existing = find_by_content_stem_column(stem);
        accept_existing(existing, /*require_same_version=*/true);
    }

    if (existing) {
        // Fold scanned provisional id + any other primary that claims this ROM stem
        // on the same system (same version only — see fold_stale_into).
        fold_stale_into(provisional_id, *existing);
        if (!stem.empty()) {
            if (auto stem_claim = find_by_content_stem_column(stem);
                stem_claim && stem_claim->game_id != existing->game_id
                && same_system(*stem_claim)
                && same_catalog_version(stem_claim->version, existing->version)) {
                fold_stale_into(stem_claim->game_id, *existing);
            }
            // Also fold path-derived primaries that still use the raw filename stem
            // as their content_stem while the survivor was edited to a cleaner stem.
            constexpr const char* kOtherStemOwners =
                "SELECT game_id FROM game_meta"
                " WHERE content_stem=? AND game_id!=? AND system_key=?;";
            sqlite3_stmt* stmt = nullptr;
            if (sqlite3_prepare_v2(db_, kOtherStemOwners, -1, &stmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_text(stmt, 1, stem.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 2, existing->game_id.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 3, existing->system_key.c_str(), -1, SQLITE_TRANSIENT);
                while (sqlite3_step(stmt) == SQLITE_ROW) {
                    const char* other = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
                    if (other != nullptr) {
                        fold_stale_into(other, *existing);
                    }
                }
                sqlite3_finalize(stmt);
            }
            (void)upsert_alias(
                game_meta_alias::kContentStem, stem, existing->system_key, existing->game_id);
            // Do not write unscoped content_stem aliases — they collide across systems.
        }

        // Path / launch metadata refresh only — identity stays DB-owned.
        existing->content_path = path_s;
        existing->content_stem =
            catalog_content_stem_for(existing->display_name, existing->version);
        if (!info.core_name.empty()) {
            existing->core_name = info.core_name;
        }
        if (!info.system_name.empty() && existing->system_name.empty()) {
            existing->system_name = info.system_name;
        }
        // game_meta.updated_at is unix seconds (human/SQL-friendly). GameInfo.updated_at
        // keeps the file-clock stamp for catalog revision.
        {
            const auto unix_mtime = file_mtime_unix_seconds(content_path);
            existing->updated_at = unix_mtime > 0 ? unix_mtime : now_epoch_seconds();
        }
        if (existing->created_at <= 0) {
            const auto created = meta_created_unix_seconds(content_path);
            existing->created_at = created > 0 ? created : existing->updated_at;
        }
        (void)upsert(*existing);
        rebuild_standard_aliases(*existing);
        apply_record_identity_to_game_info(*existing, info);

        GamePlayModesRecord modes;
        modes.game_id = existing->game_id;
        if (auto prior = find_play_modes(existing->game_id)) {
            modes = *prior;
        } else {
            modes.supports_singleplayer = info.supports_singleplayer;
            modes.supports_multiplayer = info.supports_multiplayer;
            modes.min_players = info.min_players;
            modes.max_players = info.max_players;
            modes.source = existing->source;
        }
        modes.updated_at = existing->updated_at;
        // Always refresh live catalog flags from DB play_modes when present.
        if (auto prior = find_play_modes(existing->game_id)) {
            info.supports_singleplayer = prior->supports_singleplayer;
            info.supports_multiplayer = prior->supports_multiplayer;
            info.min_players = prior->min_players;
            info.max_players = prior->max_players;
        } else {
            (void)upsert_play_modes(modes);
        }
        return;
    }

    // Brand-new title: seed DB from scan.
    GameMetaRecord row;
    row.game_id = info.id;
    row.system_key = info.system_key;
    row.system_name = info.system_name;
    row.display_name = info.display_name;
    row.canonical_name = info.canonical_name;
    row.core_name = info.core_name;
    row.asset_key = info.asset_key;
    row.identity_key = info.identity_key;
    row.version = info.version;
    row.language = info.language;
    row.region = info.region;
    row.content_stem = catalog_content_stem_for(info.display_name, info.version);
    row.content_path = path_s;
    {
        const auto unix_mtime = file_mtime_unix_seconds(content_path);
        row.updated_at = unix_mtime > 0 ? unix_mtime : now_epoch_seconds();
        const auto created = meta_created_unix_seconds(content_path);
        row.created_at = created > 0 ? created : row.updated_at;
    }
    row.source = std::string(game_meta_source::kCatalog);
    std::vector<std::pair<std::string, std::string>> extras;
    read_optional_meta_aliases(content_path, row, extras);
    if (!upsert(row)) {
        return;
    }
    rebuild_standard_aliases(row);
    for (const auto& [kind, value] : extras) {
        (void)upsert_alias(kind, value, row.system_key, row.game_id);
    }
    GamePlayModesRecord modes;
    modes.game_id = row.game_id;
    modes.supports_singleplayer = info.supports_singleplayer;
    modes.supports_multiplayer = info.supports_multiplayer;
    modes.min_players = info.min_players;
    modes.max_players = info.max_players;
    modes.updated_at = row.updated_at;
    modes.source = row.source;
    (void)upsert_play_modes(modes);
}

bool GameMetaStore::write_meta_sidecar(const GameMetaRecord& row) const {
    if (row.content_path.empty()) {
        return false;
    }
    const auto content = std::filesystem::path{row.content_path};
    auto meta_path = guess_rom_meta_json(content);
    nlohmann::json doc = nlohmann::json::object();
    std::error_code ec;
    if (!meta_path.empty() && std::filesystem::is_regular_file(meta_path, ec)) {
        try {
            std::ifstream in(meta_path);
            in >> doc;
            if (!doc.is_object()) {
                doc = nlohmann::json::object();
            }
        } catch (...) {
            doc = nlohmann::json::object();
        }
    } else {
        // Mirror Games → Meta layout when no sidecar exists yet.
        meta_path = content;
        meta_path.replace_extension(".json");
        std::vector<std::filesystem::path> parts;
        for (const auto& part : content) {
            parts.push_back(part);
        }
        for (std::size_t i = 0; i < parts.size(); ++i) {
            if (to_lower_copy(parts[i].string()) != "games") {
                continue;
            }
            std::filesystem::path rebuilt;
            for (std::size_t j = 0; j < parts.size(); ++j) {
                rebuilt /= (j == i) ? "Meta" : parts[j];
            }
            rebuilt.replace_extension(".json");
            meta_path = rebuilt;
            break;
        }
    }

    doc["name"] = row.display_name;
    if (!row.system_name.empty()) {
        doc["system_name"] = row.system_name;
    }
    if (!row.system_key.empty()) {
        doc["system_key"] = row.system_key;
    }
    if (!row.canonical_name.empty()) {
        doc["canonical_name"] = row.canonical_name;
    }
    {
        doc["version"] = catalog_version_normalize(row.version);
    }
    if (!row.language.empty()) {
        doc["language"] = row.language;
    }
    if (!row.region.empty() && row.region != "unknown") {
        doc["region"] = row.region;
    }
    if (auto modes = find_play_modes(row.game_id)) {
        doc["modes"] = {
            {"single", modes->supports_singleplayer},
            {"multi", modes->supports_multiplayer},
        };
        doc["min_players"] = modes->min_players;
        doc["max_players"] = modes->max_players;
    }

    try {
        std::filesystem::create_directories(meta_path.parent_path(), ec);
        std::ofstream out(meta_path, std::ios::trunc);
        if (!out) {
            return false;
        }
        out << doc.dump(2) << '\n';
        return true;
    } catch (...) {
        return false;
    }
}

bool GameMetaStore::learn_switch_title_id(std::string_view title_id, std::string_view title_hint) {
    if (db_ == nullptr || title_id.empty() || title_hint.empty()) {
        return false;
    }
    const auto tid = normalize_switch_title_id(title_id);
    if (tid.empty() || !looks_like_title_id(tid)) {
        return false;
    }
    if (find_by_id(tid).has_value()) {
        // title_id is never a catalog game_id; ignore.
    }
    if (const auto existing = resolve(tid, "switch")) {
        return true;
    }

    auto match_hint = [&](std::string_view hint) -> std::optional<GameMetaRecord> {
        if (hint.empty()) {
            return std::nullopt;
        }
        if (auto row = resolve(hint, "switch")) {
            return row;
        }
        // Catalog often keeps Nintendo's "… Version" suffix; Ryujinx usually does not.
        const auto with_version = std::string(hint) + " Version";
        if (auto row = resolve(with_version, "switch")) {
            return row;
        }
        // Also try other systems for non-Switch harvest (same helper).
        if (auto row = resolve(hint, {})) {
            return row;
        }
        if (auto row = resolve(with_version, {})) {
            return row;
        }
        const auto stripped = strip_trailing_version_label(std::string(hint));
        if (stripped != hint) {
            if (auto row = resolve(stripped, "switch")) {
                return row;
            }
            if (auto row = resolve(stripped + " Version", "switch")) {
                return row;
            }
        }
        const auto sanitized = sanitize_game_display_name(std::string(hint));
        if (sanitized != hint) {
            return resolve(sanitized, "switch");
        }
        return std::nullopt;
    };

    auto row = match_hint(title_hint);
    if (!row) {
        return false;
    }
    // Prefer base (unversioned) catalog entry when both exist.
    if (!catalog_version_is_base(row->version)) {
        if (auto base = match_hint(strip_trailing_version_label(row->display_name))) {
            if (catalog_version_is_base(base->version)) {
                row = std::move(base);
            }
        }
    } else {
        const auto stripped = strip_trailing_version_label(row->display_name);
        if (stripped != row->display_name) {
            if (auto base = match_hint(stripped)) {
                row = std::move(base);
            }
        }
    }

    const bool ok_scoped =
        upsert_alias(game_meta_alias::kTitleId, tid, "switch", row->game_id);
    const bool ok_global = upsert_alias(game_meta_alias::kTitleId, tid, {}, row->game_id);
    return ok_scoped || ok_global;
}

SaveNameHints GameMetaStore::save_name_hints() const {
    SaveNameHints hints;
    if (db_ == nullptr) {
        return hints;
    }
    constexpr const char* kSql =
        "SELECT a.alias_value, m.system_key, m.display_name"
        " FROM game_aliases a"
        " JOIN game_meta m ON m.game_id = a.game_id;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, kSql, -1, &stmt, nullptr) != SQLITE_OK) {
        return hints;
    }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const auto* value = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        const auto* system = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        const auto* display = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        if (value == nullptr || display == nullptr || *display == '\0') {
            continue;
        }
        hints.by_stem[value] = {
            system != nullptr ? std::string(system) : std::string{},
            display,
        };
    }
    sqlite3_finalize(stmt);

    // Also index by catalog id and content stem from the primary table.
    constexpr const char* kMeta =
        "SELECT game_id, system_key, display_name, content_stem, canonical_name FROM game_meta;";
    if (sqlite3_prepare_v2(db_, kMeta, -1, &stmt, nullptr) != SQLITE_OK) {
        return hints;
    }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const auto* id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        const auto* system = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        const auto* display = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        const auto* stem = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        const auto* canonical = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        if (display == nullptr || *display == '\0') {
            continue;
        }
        const std::string system_s = system != nullptr ? system : "";
        const std::string display_s = display;
        if (id != nullptr && *id != '\0') {
            hints.by_stem[id] = {system_s, display_s};
        }
        if (stem != nullptr && *stem != '\0') {
            hints.by_stem[stem] = {system_s, display_s};
            const auto stem_base = save_match_base_name(stem);
            if (!stem_base.empty() && stem_base != stem) {
                hints.by_stem[stem_base] = {system_s, display_s};
            }
        }
        if (canonical != nullptr && *canonical != '\0') {
            hints.by_stem[to_lower_copy(canonical)] = {system_s, display_s};
        }
        hints.by_stem[to_lower_copy(display_s)] = {system_s, display_s};
        const auto display_base = save_match_base_name(display_s);
        if (!display_base.empty() && display_base != to_lower_copy(display_s)) {
            hints.by_stem[display_base] = {system_s, display_s};
        }
    }
    sqlite3_finalize(stmt);
    return hints;
}

std::vector<GameMetaRecord> GameMetaStore::list_games() const {
    std::vector<GameMetaRecord> out;
    if (db_ == nullptr) {
        return out;
    }
    constexpr const char* kSql =
        "SELECT game_id, system_key, system_name, display_name, canonical_name, core_name,"
        " asset_key, identity_key, version, language, region, content_stem,"
        " created_at, updated_at, source, content_path"
        " FROM game_meta"
        " ORDER BY system_key COLLATE NOCASE, display_name COLLATE NOCASE, game_id;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, kSql, -1, &stmt, nullptr) != SQLITE_OK) {
        return out;
    }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        out.push_back(row_from_stmt(stmt));
    }
    sqlite3_finalize(stmt);
    return out;
}

std::vector<GameMetaAliasRecord> GameMetaStore::list_aliases(std::string_view game_id) const {
    std::vector<GameMetaAliasRecord> out;
    if (db_ == nullptr) {
        return out;
    }
    const char* sql = nullptr;
    if (!game_id.empty()) {
        sql = "SELECT alias_kind, alias_value, system_key, game_id FROM game_aliases"
              " WHERE game_id=?"
              " ORDER BY alias_kind COLLATE NOCASE, system_key COLLATE NOCASE,"
              " alias_value COLLATE NOCASE;";
    } else {
        sql = "SELECT alias_kind, alias_value, system_key, game_id FROM game_aliases"
              " ORDER BY game_id, alias_kind COLLATE NOCASE, system_key COLLATE NOCASE,"
              " alias_value COLLATE NOCASE;";
    }
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return out;
    }
    if (!game_id.empty()) {
        const auto id = std::string(game_id);
        sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
    }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        GameMetaAliasRecord row;
        const auto* kind = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        const auto* value = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        const auto* system = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        const auto* id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        row.alias_kind = kind != nullptr ? kind : "";
        row.alias_value = value != nullptr ? value : "";
        row.system_key = system != nullptr ? system : "";
        row.game_id = id != nullptr ? id : "";
        out.push_back(std::move(row));
    }
    sqlite3_finalize(stmt);
    return out;
}

bool GameMetaStore::record_user_game(
    std::string_view username,
    std::string_view game_id,
    std::string_view system_key) {
    if (db_ == nullptr || username.empty() || game_id.empty()) {
        return false;
    }
    constexpr const char* kSql =
        "INSERT INTO user_games (username, game_id, system_key, last_played_at)"
        " VALUES (?,?,?,?)"
        " ON CONFLICT(username, game_id) DO UPDATE SET"
        "  system_key=excluded.system_key,"
        "  last_played_at=excluded.last_played_at;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, kSql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    const auto user_s = std::string(username);
    const auto game_s = std::string(game_id);
    const auto system_s = std::string(system_key);
    const auto now = now_epoch_seconds();
    sqlite3_bind_text(stmt, 1, user_s.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, game_s.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, system_s.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 4, now);
    const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

bool GameMetaStore::remove_user_game(std::string_view username, std::string_view game_id) {
    if (db_ == nullptr || username.empty() || game_id.empty()) {
        return false;
    }
    constexpr const char* kSql = "DELETE FROM user_games WHERE username=? AND game_id=?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, kSql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    const auto user_s = std::string(username);
    const auto game_s = std::string(game_id);
    sqlite3_bind_text(stmt, 1, user_s.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, game_s.c_str(), -1, SQLITE_TRANSIENT);
    const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

std::size_t GameMetaStore::remove_user_games_for_system(
    std::string_view username,
    std::string_view system_key) {
    if (db_ == nullptr || username.empty() || system_key.empty()) {
        return 0;
    }
    constexpr const char* kSql = "DELETE FROM user_games WHERE username=? AND system_key=?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, kSql, -1, &stmt, nullptr) != SQLITE_OK) {
        return 0;
    }
    const auto user_s = std::string(username);
    const auto system_s = std::string(system_key);
    sqlite3_bind_text(stmt, 1, user_s.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, system_s.c_str(), -1, SQLITE_TRANSIENT);
    const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    const auto changes = ok ? static_cast<std::size_t>(sqlite3_changes(db_)) : 0;
    sqlite3_finalize(stmt);
    return changes;
}

std::size_t GameMetaStore::migrate_user_games_game_id(
    std::string_view old_game_id,
    std::string_view new_game_id) {
    if (db_ == nullptr || old_game_id.empty() || new_game_id.empty()
        || old_game_id == new_game_id) {
        return 0;
    }
    // Rows that already have new_game_id: drop the conflicting old rows.
    constexpr const char* kDeleteDup =
        "DELETE FROM user_games WHERE game_id=? AND username IN ("
        " SELECT username FROM ("
        "  SELECT username FROM user_games WHERE game_id=?"
        " )"
        ");";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, kDeleteDup, -1, &stmt, nullptr) == SQLITE_OK) {
        const auto old_s = std::string(old_game_id);
        const auto new_s = std::string(new_game_id);
        sqlite3_bind_text(stmt, 1, old_s.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, new_s.c_str(), -1, SQLITE_TRANSIENT);
        (void)sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
    constexpr const char* kUpdate = "UPDATE user_games SET game_id=? WHERE game_id=?;";
    if (sqlite3_prepare_v2(db_, kUpdate, -1, &stmt, nullptr) != SQLITE_OK) {
        return 0;
    }
    const auto new_s = std::string(new_game_id);
    const auto old_s = std::string(old_game_id);
    sqlite3_bind_text(stmt, 1, new_s.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, old_s.c_str(), -1, SQLITE_TRANSIENT);
    const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    const auto changes = ok ? static_cast<std::size_t>(sqlite3_changes(db_)) : 0;
    sqlite3_finalize(stmt);
    return changes;
}

std::size_t GameMetaStore::remove_user_games_for_game_id(std::string_view game_id) {
    if (db_ == nullptr || game_id.empty()) {
        return 0;
    }
    constexpr const char* kSql = "DELETE FROM user_games WHERE game_id=?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, kSql, -1, &stmt, nullptr) != SQLITE_OK) {
        return 0;
    }
    const auto id = std::string(game_id);
    sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
    const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    const auto changes = ok ? static_cast<std::size_t>(sqlite3_changes(db_)) : 0;
    sqlite3_finalize(stmt);
    return changes;
}

bool GameMetaStore::delete_game(std::string_view game_id) {
    if (db_ == nullptr || game_id.empty()) {
        return false;
    }
    (void)delete_play_modes(game_id);
    constexpr const char* kSql = "DELETE FROM game_meta WHERE game_id=?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, kSql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    const auto id = std::string(game_id);
    sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
    const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

std::size_t GameMetaStore::delete_aliases_for_game(std::string_view game_id) {
    if (db_ == nullptr || game_id.empty()) {
        return 0;
    }
    constexpr const char* kSql = "DELETE FROM game_aliases WHERE game_id=?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, kSql, -1, &stmt, nullptr) != SQLITE_OK) {
        return 0;
    }
    const auto id = std::string(game_id);
    sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
    const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    const auto changes = ok ? static_cast<std::size_t>(sqlite3_changes(db_)) : 0;
    sqlite3_finalize(stmt);
    return changes;
}

bool GameMetaStore::delete_alias(
    std::string_view alias_kind,
    std::string_view alias_value,
    std::string_view system_key) {
    if (db_ == nullptr || alias_kind.empty() || alias_value.empty()) {
        return false;
    }
    const auto normalized = normalize_alias_value(alias_kind, alias_value);
    if (normalized.empty()) {
        return false;
    }
    constexpr const char* kSql =
        "DELETE FROM game_aliases WHERE alias_kind=? AND alias_value=? AND system_key=?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, kSql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    const auto kind_s = std::string(alias_kind);
    const auto system_s = std::string(system_key);
    sqlite3_bind_text(stmt, 1, kind_s.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, normalized.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, system_s.c_str(), -1, SQLITE_TRANSIENT);
    const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

std::size_t GameMetaStore::reassign_aliases_game_id(
    std::string_view old_game_id,
    std::string_view new_game_id) {
    if (db_ == nullptr || old_game_id.empty() || new_game_id.empty()
        || old_game_id == new_game_id) {
        return 0;
    }
    constexpr const char* kSql = "UPDATE game_aliases SET game_id=? WHERE game_id=?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, kSql, -1, &stmt, nullptr) != SQLITE_OK) {
        return 0;
    }
    const auto new_s = std::string(new_game_id);
    const auto old_s = std::string(old_game_id);
    sqlite3_bind_text(stmt, 1, new_s.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, old_s.c_str(), -1, SQLITE_TRANSIENT);
    const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    const auto changes = ok ? static_cast<std::size_t>(sqlite3_changes(db_)) : 0;
    sqlite3_finalize(stmt);
    return changes;
}

void GameMetaStore::rebuild_standard_aliases(const GameMetaRecord& row) {
    if (db_ == nullptr || row.game_id.empty()) {
        return;
    }
    (void)delete_aliases_for_game(row.game_id);
    const auto& sys = row.system_key;
    (void)upsert_alias(game_meta_alias::kCatalogId, row.game_id, {}, row.game_id);
    (void)upsert_alias(game_meta_alias::kCatalogId, row.game_id, sys, row.game_id);
    // Name/stem aliases stay system-scoped so identically named titles on different
    // systems (e.g. Final Fantasy III on GBA vs NDS) do not collapse.
    // Bare display/canonical aliases are base-only — versioned builds share those
    // names and must stay distinct (matched via content_path / content_stem / id).
    const bool base_version = catalog_version_is_base(row.version);
    if (!row.canonical_name.empty() && base_version) {
        (void)upsert_alias(game_meta_alias::kCanonical, row.canonical_name, sys, row.game_id);
    }
    if (!row.content_stem.empty()) {
        (void)upsert_alias(game_meta_alias::kContentStem, row.content_stem, sys, row.game_id);
        if (sys == "switch" && looks_like_title_id(row.content_stem)) {
            (void)upsert_alias(game_meta_alias::kTitleId, row.content_stem, sys, row.game_id);
            (void)upsert_alias(game_meta_alias::kTitleId, row.content_stem, {}, row.game_id);
        }
        const auto stem_base = save_match_base_name(row.content_stem);
        if (base_version && !stem_base.empty() && stem_base != row.content_stem) {
            (void)upsert_alias(game_meta_alias::kContentStem, stem_base, sys, row.game_id);
        }
    }
    if (!row.display_name.empty() && base_version) {
        (void)upsert_alias(game_meta_alias::kDisplayName, row.display_name, sys, row.game_id);
        const auto display_base = save_match_base_name(row.display_name);
        if (!display_base.empty()
            && display_base != to_lower_copy(fold_common_latin_accents(row.display_name))) {
            (void)upsert_alias(game_meta_alias::kDisplayName, display_base, sys, row.game_id);
        }
    }
}

std::vector<UserGameRecord> GameMetaStore::list_user_games(
    std::string_view username,
    std::string_view system_key) const {
    std::vector<UserGameRecord> out;
    if (db_ == nullptr) {
        return out;
    }
    const char* sql = nullptr;
    if (!username.empty() && !system_key.empty()) {
        sql = "SELECT username, game_id, system_key, last_played_at FROM user_games"
              " WHERE username=? AND system_key=? ORDER BY last_played_at DESC;";
    } else if (!username.empty()) {
        sql = "SELECT username, game_id, system_key, last_played_at FROM user_games"
              " WHERE username=? ORDER BY last_played_at DESC;";
    } else if (!system_key.empty()) {
        sql = "SELECT username, game_id, system_key, last_played_at FROM user_games"
              " WHERE system_key=? ORDER BY last_played_at DESC;";
    } else {
        sql = "SELECT username, game_id, system_key, last_played_at FROM user_games"
              " ORDER BY last_played_at DESC;";
    }
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return out;
    }
    int bind = 1;
    const auto user_s = std::string(username);
    const auto system_s = std::string(system_key);
    if (!username.empty()) {
        sqlite3_bind_text(stmt, bind++, user_s.c_str(), -1, SQLITE_TRANSIENT);
    }
    if (!system_key.empty()) {
        sqlite3_bind_text(stmt, bind++, system_s.c_str(), -1, SQLITE_TRANSIENT);
    }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        UserGameRecord row;
        row.username = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        row.game_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        row.system_key = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        row.last_played_at = sqlite3_column_int64(stmt, 3);
        out.push_back(std::move(row));
    }
    sqlite3_finalize(stmt);
    return out;
}

bool GameMetaStore::upsert_play_modes(const GamePlayModesRecord& row) {
    if (db_ == nullptr || row.game_id.empty()) {
        return false;
    }
    constexpr const char* kSql =
        "INSERT INTO game_play_modes ("
        "  game_id, supports_single, supports_multi, min_players, max_players, updated_at, source"
        ") VALUES (?, ?, ?, ?, ?, ?, ?)"
        " ON CONFLICT(game_id) DO UPDATE SET"
        "  supports_single=excluded.supports_single,"
        "  supports_multi=excluded.supports_multi,"
        "  min_players=excluded.min_players,"
        "  max_players=excluded.max_players,"
        "  updated_at=excluded.updated_at,"
        "  source=excluded.source;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, kSql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_text(stmt, 1, row.game_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, row.supports_singleplayer ? 1 : 0);
    sqlite3_bind_int(stmt, 3, row.supports_multiplayer ? 1 : 0);
    sqlite3_bind_int(stmt, 4, static_cast<int>(row.min_players));
    sqlite3_bind_int(stmt, 5, static_cast<int>(row.max_players));
    sqlite3_bind_int64(stmt, 6, row.updated_at);
    sqlite3_bind_text(stmt, 7, row.source.c_str(), -1, SQLITE_TRANSIENT);
    const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

std::optional<GamePlayModesRecord> GameMetaStore::find_play_modes(std::string_view game_id) const {
    if (db_ == nullptr || game_id.empty()) {
        return std::nullopt;
    }
    constexpr const char* kSql =
        "SELECT game_id, supports_single, supports_multi, min_players, max_players, updated_at, source"
        " FROM game_play_modes WHERE game_id=?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, kSql, -1, &stmt, nullptr) != SQLITE_OK) {
        return std::nullopt;
    }
    const auto id = std::string(game_id);
    sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
    std::optional<GamePlayModesRecord> out;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        GamePlayModesRecord row;
        row.game_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        row.supports_singleplayer = sqlite3_column_int(stmt, 1) != 0;
        row.supports_multiplayer = sqlite3_column_int(stmt, 2) != 0;
        row.min_players = static_cast<std::uint8_t>(sqlite3_column_int(stmt, 3));
        row.max_players = static_cast<std::uint8_t>(sqlite3_column_int(stmt, 4));
        row.updated_at = sqlite3_column_int64(stmt, 5);
        row.source = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
        out = std::move(row);
    }
    sqlite3_finalize(stmt);
    return out;
}

std::vector<GamePlayModesRecord> GameMetaStore::list_play_modes() const {
    std::vector<GamePlayModesRecord> out;
    if (db_ == nullptr) {
        return out;
    }
    constexpr const char* kSql =
        "SELECT m.game_id, m.supports_single, m.supports_multi, m.min_players, m.max_players,"
        " m.updated_at, m.source"
        " FROM game_play_modes m"
        " LEFT JOIN game_meta g ON g.game_id = m.game_id"
        " ORDER BY COALESCE(g.system_key, ''), COALESCE(g.display_name, m.game_id);";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, kSql, -1, &stmt, nullptr) != SQLITE_OK) {
        return out;
    }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        GamePlayModesRecord row;
        row.game_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        row.supports_singleplayer = sqlite3_column_int(stmt, 1) != 0;
        row.supports_multiplayer = sqlite3_column_int(stmt, 2) != 0;
        row.min_players = static_cast<std::uint8_t>(sqlite3_column_int(stmt, 3));
        row.max_players = static_cast<std::uint8_t>(sqlite3_column_int(stmt, 4));
        row.updated_at = sqlite3_column_int64(stmt, 5);
        row.source = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
        out.push_back(std::move(row));
    }
    sqlite3_finalize(stmt);
    return out;
}

bool GameMetaStore::migrate_play_modes_game_id(
    std::string_view old_game_id,
    std::string_view new_game_id) {
    if (db_ == nullptr || old_game_id.empty() || new_game_id.empty()
        || old_game_id == new_game_id) {
        return false;
    }
    auto existing = find_play_modes(old_game_id);
    if (!existing) {
        return false;
    }
    if (find_play_modes(new_game_id)) {
        (void)delete_play_modes(old_game_id);
        return true;
    }
    existing->game_id = std::string(new_game_id);
    if (!upsert_play_modes(*existing)) {
        return false;
    }
    (void)delete_play_modes(old_game_id);
    return true;
}

bool GameMetaStore::delete_play_modes(std::string_view game_id) {
    if (db_ == nullptr || game_id.empty()) {
        return false;
    }
    constexpr const char* kSql = "DELETE FROM game_play_modes WHERE game_id=?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, kSql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    const auto id = std::string(game_id);
    sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
    const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

std::shared_ptr<GameMetaStore> make_game_meta_store() {
    return std::make_shared<GameMetaStore>();
}

void sync_game_meta_from_catalog(const GameCatalog& catalog) {
    try {
        GameMetaStore store;
        if (store.ready()) {
            store.sync_from_catalog(catalog);
        }
    } catch (const std::exception& error) {
        std::cerr << "game_meta: sync failed: " << error.what() << '\n';
    } catch (...) {
        std::cerr << "game_meta: sync failed\n";
    }
}

void record_user_game_played(
    std::string_view username,
    std::string_view game_id,
    std::string_view system_key) {
    try {
        GameMetaStore store;
        if (store.ready()) {
            (void)store.record_user_game(username, game_id, system_key);
        }
    } catch (...) {
        // Soft-fail.
    }
}

bool GameMetaStore::block_user_game(
    std::string_view username,
    std::string_view game_id,
    std::string_view system_key) {
    if (db_ == nullptr || username.empty() || game_id.empty()) {
        return false;
    }
    std::string system_s{system_key};
    if (system_s.empty()) {
        if (const auto row = find_by_id(game_id)) {
            system_s = row->system_key;
        }
    }
    constexpr const char* kSql =
        "INSERT INTO user_game_blocks (username, game_id, system_key, created_at)"
        " VALUES (?,?,?,?)"
        " ON CONFLICT(username, game_id) DO UPDATE SET"
        "  system_key=excluded.system_key;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, kSql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    const auto user_s = std::string(username);
    const auto game_s = std::string(game_id);
    const auto now = now_epoch_seconds();
    sqlite3_bind_text(stmt, 1, user_s.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, game_s.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, system_s.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 4, now);
    const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

bool GameMetaStore::unblock_user_game(std::string_view username, std::string_view game_id) {
    if (db_ == nullptr || username.empty() || game_id.empty()) {
        return false;
    }
    constexpr const char* kSql = "DELETE FROM user_game_blocks WHERE username=? AND game_id=?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, kSql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    const auto user_s = std::string(username);
    const auto game_s = std::string(game_id);
    sqlite3_bind_text(stmt, 1, user_s.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, game_s.c_str(), -1, SQLITE_TRANSIENT);
    const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

bool GameMetaStore::is_user_game_blocked(
    std::string_view username,
    std::string_view game_id) const {
    if (db_ == nullptr || username.empty() || game_id.empty()) {
        return false;
    }
    constexpr const char* kSql =
        "SELECT 1 FROM user_game_blocks WHERE username=? AND game_id=? LIMIT 1;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, kSql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    const auto user_s = std::string(username);
    const auto game_s = std::string(game_id);
    sqlite3_bind_text(stmt, 1, user_s.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, game_s.c_str(), -1, SQLITE_TRANSIENT);
    const bool blocked = sqlite3_step(stmt) == SQLITE_ROW;
    sqlite3_finalize(stmt);
    return blocked;
}

std::vector<UserGameBlockRecord> GameMetaStore::list_user_game_blocks(
    std::string_view username) const {
    std::vector<UserGameBlockRecord> out;
    if (db_ == nullptr) {
        return out;
    }
    const char* sql = username.empty()
        ? "SELECT b.username, b.game_id, b.system_key, b.created_at,"
          " COALESCE(NULLIF(m.display_name,''), NULLIF(m.canonical_name,''), b.game_id)"
          " FROM user_game_blocks b"
          " LEFT JOIN game_meta m ON m.game_id=b.game_id"
          " ORDER BY b.username COLLATE NOCASE, 5 COLLATE NOCASE;"
        : "SELECT b.username, b.game_id, b.system_key, b.created_at,"
          " COALESCE(NULLIF(m.display_name,''), NULLIF(m.canonical_name,''), b.game_id)"
          " FROM user_game_blocks b"
          " LEFT JOIN game_meta m ON m.game_id=b.game_id"
          " WHERE b.username=?"
          " ORDER BY 5 COLLATE NOCASE;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return out;
    }
    if (!username.empty()) {
        const auto user_s = std::string(username);
        sqlite3_bind_text(stmt, 1, user_s.c_str(), -1, SQLITE_TRANSIENT);
    }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        UserGameBlockRecord row;
        const auto* user = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        const auto* id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        const auto* system = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        row.created_at = sqlite3_column_int64(stmt, 3);
        const auto* display = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        row.username = user != nullptr ? user : "";
        row.game_id = id != nullptr ? id : "";
        row.system_key = system != nullptr ? system : "";
        row.display_name = display != nullptr ? display : row.game_id;
        out.push_back(std::move(row));
    }
    sqlite3_finalize(stmt);
    return out;
}

std::size_t GameMetaStore::migrate_user_game_blocks_game_id(
    std::string_view old_game_id,
    std::string_view new_game_id) {
    if (db_ == nullptr || old_game_id.empty() || new_game_id.empty()
        || old_game_id == new_game_id) {
        return 0;
    }
    constexpr const char* kDeleteDup =
        "DELETE FROM user_game_blocks WHERE game_id=? AND username IN ("
        " SELECT username FROM ("
        "  SELECT username FROM user_game_blocks WHERE game_id=?"
        " )"
        ");";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, kDeleteDup, -1, &stmt, nullptr) == SQLITE_OK) {
        const auto old_s = std::string(old_game_id);
        const auto new_s = std::string(new_game_id);
        sqlite3_bind_text(stmt, 1, old_s.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, new_s.c_str(), -1, SQLITE_TRANSIENT);
        (void)sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
    constexpr const char* kUpdate = "UPDATE user_game_blocks SET game_id=? WHERE game_id=?;";
    if (sqlite3_prepare_v2(db_, kUpdate, -1, &stmt, nullptr) != SQLITE_OK) {
        return 0;
    }
    const auto new_s = std::string(new_game_id);
    const auto old_s = std::string(old_game_id);
    sqlite3_bind_text(stmt, 1, new_s.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, old_s.c_str(), -1, SQLITE_TRANSIENT);
    const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    const auto changes = ok ? static_cast<std::size_t>(sqlite3_changes(db_)) : 0;
    sqlite3_finalize(stmt);
    return changes;
}

std::size_t GameMetaStore::remove_user_game_blocks_for_game_id(std::string_view game_id) {
    if (db_ == nullptr || game_id.empty()) {
        return 0;
    }
    constexpr const char* kSql = "DELETE FROM user_game_blocks WHERE game_id=?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, kSql, -1, &stmt, nullptr) != SQLITE_OK) {
        return 0;
    }
    const auto game_s = std::string(game_id);
    sqlite3_bind_text(stmt, 1, game_s.c_str(), -1, SQLITE_TRANSIENT);
    const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    const auto changes = ok ? static_cast<std::size_t>(sqlite3_changes(db_)) : 0;
    sqlite3_finalize(stmt);
    return changes;
}

namespace {

std::uint64_t revision_from_content_hash(std::string_view hash) {
    // hash is "sha256:" + 64 hex digits — fold the first 16 nibbles into a u64.
    constexpr std::string_view kPrefix = "sha256:";
    if (hash.size() <= kPrefix.size() || hash.substr(0, kPrefix.size()) != kPrefix) {
        return 1;
    }
    const auto hex = hash.substr(kPrefix.size());
    std::uint64_t value = 0;
    const auto take = std::min<std::size_t>(16, hex.size());
    for (std::size_t i = 0; i < take; ++i) {
        const auto c = static_cast<unsigned char>(hex[i]);
        std::uint8_t nibble = 0;
        if (c >= '0' && c <= '9') {
            nibble = static_cast<std::uint8_t>(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            nibble = static_cast<std::uint8_t>(10 + c - 'a');
        } else if (c >= 'A' && c <= 'F') {
            nibble = static_cast<std::uint8_t>(10 + c - 'A');
        } else {
            continue;
        }
        value = (value << 4) | nibble;
    }
    return value == 0 ? 1 : value;
}

std::string playlist_discs_to_json(const std::vector<std::string>& discs) {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& disc : discs) {
        arr.push_back(disc);
    }
    return arr.dump();
}

std::vector<std::string> playlist_discs_from_json(std::string_view json_text) {
    std::vector<std::string> out;
    if (json_text.empty()) {
        return out;
    }
    try {
        const auto parsed = nlohmann::json::parse(json_text);
        if (!parsed.is_array()) {
            return out;
        }
        for (const auto& item : parsed) {
            if (item.is_string()) {
                out.push_back(item.get<std::string>());
            }
        }
    } catch (...) {
    }
    return out;
}

} // namespace

std::vector<GameId> GameMetaStore::list_blocked_game_ids(std::string_view username) const {
    std::vector<GameId> out;
    if (db_ == nullptr || username.empty()) {
        return out;
    }
    constexpr const char* kSql =
        "SELECT game_id FROM user_game_blocks WHERE username=? ORDER BY game_id;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, kSql, -1, &stmt, nullptr) != SQLITE_OK) {
        return out;
    }
    const auto user_s = std::string(username);
    sqlite3_bind_text(stmt, 1, user_s.c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        if (text != nullptr && text[0] != '\0') {
            out.emplace_back(text);
        }
    }
    sqlite3_finalize(stmt);
    return out;
}

std::uint64_t GameMetaStore::user_blocks_revision(std::string_view username) const {
    const auto ids = list_blocked_game_ids(username);
    std::string blob;
    blob.reserve(ids.size() * 64);
    for (const auto& id : ids) {
        blob.append(id);
        blob.push_back('\n');
    }
    return revision_from_content_hash(sha256_hex(blob));
}

CatalogUserBlocks GameMetaStore::catalog_user_blocks_for(
    std::string_view username,
    std::uint64_t client_blocks_revision) const {
    CatalogUserBlocks blocks;
    const auto ids = list_blocked_game_ids(username);
    std::string blob;
    blob.reserve(ids.size() * 64);
    for (const auto& id : ids) {
        blob.append(id);
        blob.push_back('\n');
    }
    blocks.blocks_revision = revision_from_content_hash(sha256_hex(blob));
    if (client_blocks_revision != 0 && client_blocks_revision == blocks.blocks_revision) {
        blocks.full = false;
        return blocks;
    }
    blocks.full = true;
    blocks.blocked_game_ids = ids;
    return blocks;
}

bool GameMetaStore::rebuild_catalog_offerings(const GameList& list) {
    if (db_ == nullptr) {
        return false;
    }

    GameList canonical = list;
    canonical.full = true;
    canonical.deleted_game_ids.clear();
    std::sort(canonical.games.begin(), canonical.games.end(), [](const GameInfo& a, const GameInfo& b) {
        return a.id < b.id;
    });
    for (auto& game : canonical.games) {
        game.version = catalog_version_display_token(game.version);
    }
    // Hash must not include revision (revision is derived from the hash).
    canonical.catalog_revision = 0;
    const auto payload_bytes = serialize_payload(canonical);
    const auto content_hash = sha256_hex(
        std::string_view(
            reinterpret_cast<const char*>(payload_bytes.data()),
            payload_bytes.size()));
    const auto revision = revision_from_content_hash(content_hash);
    canonical.catalog_revision = revision;

    if (!exec("BEGIN IMMEDIATE;")) {
        return false;
    }
    if (!exec("DELETE FROM catalog_offerings;")) {
        (void)exec("ROLLBACK;");
        return false;
    }

    constexpr const char* kInsert =
        "INSERT INTO catalog_offerings ("
        "  game_id, identity_key, asset_key, display_name, system_name, system_key,"
        "  core_name, canonical_name, version, language, region,"
        "  supports_single, supports_multi, min_players, max_players, updated_at,"
        "  playlist_discs_json"
        ") VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);";
    sqlite3_stmt* insert = nullptr;
    if (sqlite3_prepare_v2(db_, kInsert, -1, &insert, nullptr) != SQLITE_OK) {
        (void)exec("ROLLBACK;");
        return false;
    }
    for (const auto& game : canonical.games) {
        sqlite3_reset(insert);
        sqlite3_clear_bindings(insert);
        const auto discs_json = playlist_discs_to_json(game.playlist_discs);
        sqlite3_bind_text(insert, 1, game.id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(insert, 2, game.identity_key.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(insert, 3, game.asset_key.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(insert, 4, game.display_name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(insert, 5, game.system_name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(insert, 6, game.system_key.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(insert, 7, game.core_name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(insert, 8, game.canonical_name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(insert, 9, game.version.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(insert, 10, game.language.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(insert, 11, game.region.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(insert, 12, game.supports_singleplayer ? 1 : 0);
        sqlite3_bind_int(insert, 13, game.supports_multiplayer ? 1 : 0);
        sqlite3_bind_int(insert, 14, game.min_players);
        sqlite3_bind_int(insert, 15, game.max_players);
        sqlite3_bind_int64(insert, 16, static_cast<sqlite3_int64>(game.updated_at));
        sqlite3_bind_text(insert, 17, discs_json.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(insert) != SQLITE_DONE) {
            sqlite3_finalize(insert);
            (void)exec("ROLLBACK;");
            return false;
        }
    }
    sqlite3_finalize(insert);

    constexpr const char* kState =
        "INSERT INTO catalog_offerings_state (id, content_hash, revision, game_count, rebuilt_at)"
        " VALUES (1, ?, ?, ?, ?)"
        " ON CONFLICT(id) DO UPDATE SET"
        "  content_hash=excluded.content_hash,"
        "  revision=excluded.revision,"
        "  game_count=excluded.game_count,"
        "  rebuilt_at=excluded.rebuilt_at;";
    sqlite3_stmt* state = nullptr;
    if (sqlite3_prepare_v2(db_, kState, -1, &state, nullptr) != SQLITE_OK) {
        (void)exec("ROLLBACK;");
        return false;
    }
    const auto now = static_cast<sqlite3_int64>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    sqlite3_bind_text(state, 1, content_hash.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(state, 2, static_cast<sqlite3_int64>(revision));
    sqlite3_bind_int64(state, 3, static_cast<sqlite3_int64>(canonical.games.size()));
    sqlite3_bind_int64(state, 4, now);
    const bool state_ok = sqlite3_step(state) == SQLITE_DONE;
    sqlite3_finalize(state);
    if (!state_ok) {
        (void)exec("ROLLBACK;");
        return false;
    }
    if (!exec("COMMIT;")) {
        (void)exec("ROLLBACK;");
        return false;
    }
    std::cout
        << "game_meta: catalog_offerings rebuilt ("
        << canonical.games.size() << " game(s), rev " << revision << ")\n";
    return true;
}

GameList GameMetaStore::load_catalog_offerings() const {
    GameList out;
    out.full = true;
    if (db_ == nullptr) {
        return out;
    }
    out.catalog_revision = catalog_offerings_revision();
    constexpr const char* kSql =
        "SELECT game_id, identity_key, asset_key, display_name, system_name, system_key,"
        " core_name, canonical_name, version, language, region,"
        " supports_single, supports_multi, min_players, max_players, updated_at,"
        " playlist_discs_json"
        " FROM catalog_offerings ORDER BY game_id;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, kSql, -1, &stmt, nullptr) != SQLITE_OK) {
        return out;
    }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        GameInfo game;
        auto text_at = [&](int col) -> std::string {
            const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, col));
            return text != nullptr ? std::string(text) : std::string{};
        };
        game.id = text_at(0);
        game.identity_key = text_at(1);
        game.asset_key = text_at(2);
        game.display_name = text_at(3);
        game.system_name = text_at(4);
        game.system_key = text_at(5);
        game.core_name = text_at(6);
        game.canonical_name = text_at(7);
        game.version = text_at(8);
        game.language = text_at(9);
        game.region = text_at(10);
        game.supports_singleplayer = sqlite3_column_int(stmt, 11) != 0;
        game.supports_multiplayer = sqlite3_column_int(stmt, 12) != 0;
        game.min_players = static_cast<std::uint8_t>(sqlite3_column_int(stmt, 13));
        game.max_players = static_cast<std::uint8_t>(sqlite3_column_int(stmt, 14));
        game.updated_at = static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 15));
        game.playlist_discs = playlist_discs_from_json(text_at(16));
        out.games.push_back(std::move(game));
    }
    sqlite3_finalize(stmt);
    return out;
}

std::uint64_t GameMetaStore::catalog_offerings_revision() const {
    if (db_ == nullptr) {
        return 0;
    }
    constexpr const char* kSql = "SELECT revision FROM catalog_offerings_state WHERE id=1;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, kSql, -1, &stmt, nullptr) != SQLITE_OK) {
        return 0;
    }
    std::uint64_t revision = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        revision = static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 0));
    }
    sqlite3_finalize(stmt);
    return revision;
}

std::string GameMetaStore::catalog_offerings_content_hash() const {
    if (db_ == nullptr) {
        return {};
    }
    constexpr const char* kSql = "SELECT content_hash FROM catalog_offerings_state WHERE id=1;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, kSql, -1, &stmt, nullptr) != SQLITE_OK) {
        return {};
    }
    std::string hash;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        if (text != nullptr) {
            hash = text;
        }
    }
    sqlite3_finalize(stmt);
    return hash;
}

GameList filter_game_list_for_user(GameList list, std::string_view username) {
    if (username.empty() || list.games.empty()) {
        return list;
    }
    try {
        GameMetaStore store;
        if (!store.ready()) {
            return list;
        }
        apply_blocked_game_ids(list, store.list_blocked_game_ids(username));
    } catch (...) {
        // Soft-fail: return unfiltered.
    }
    return list;
}

void rebuild_catalog_offerings_from_list(const GameList& list) {
    try {
        GameMetaStore store;
        if (!store.ready()) {
            return;
        }
        (void)store.rebuild_catalog_offerings(list);
    } catch (...) {
    }
}

void apply_blocked_game_ids(GameList& list, const std::vector<GameId>& blocked_game_ids) {
    if (blocked_game_ids.empty() || list.games.empty()) {
        return;
    }
    std::unordered_set<std::string> blocked(
        blocked_game_ids.begin(), blocked_game_ids.end());
    list.games.erase(
        std::remove_if(
            list.games.begin(),
            list.games.end(),
            [&](const GameInfo& game) { return blocked.contains(game.id); }),
        list.games.end());
}

} // namespace archstreamer
