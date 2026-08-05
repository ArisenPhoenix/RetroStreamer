#include "host/game_meta_store.hpp"

#include "common/platform/paths.hpp"
#include "host/game_catalog_scanner.hpp"
#include "host/switch_save_share.hpp"

#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <cctype>
#include <chrono>
#include <fstream>
#include <iostream>
#include <system_error>
#include <utility>
#include <vector>

namespace archstreamer {
namespace {

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
    row.updated_at = sqlite3_column_int64(stmt, 12);
    row.source = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 13));
    if (sqlite3_column_count(stmt) > 14 && sqlite3_column_text(stmt, 14) != nullptr) {
        row.content_path = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 14));
    }
    return row;
}

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
        const auto token = to_lower_copy(parts[i].string());
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
        "CREATE INDEX IF NOT EXISTS idx_game_meta_content_path ON game_meta(content_path);");
    (void)exec_quiet(
        "CREATE INDEX IF NOT EXISTS idx_game_meta_content_stem ON game_meta(content_stem);");
    (void)exec_quiet(
        "CREATE INDEX IF NOT EXISTS idx_user_games_user_system"
        " ON user_games(username, system_key);");
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
        "  asset_key, identity_key, version, language, region, content_stem, updated_at, source,"
        "  content_path"
        ") VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)"
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
        "  updated_at=excluded.updated_at,"
        "  source=excluded.source,"
        "  content_path=excluded.content_path;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, kSql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    const auto updated =
        row.updated_at > 0 ? row.updated_at : now_epoch_seconds();
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
    sqlite3_bind_int64(stmt, 13, updated);
    sqlite3_bind_text(stmt, 14, row.source.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 15, row.content_path.c_str(), -1, SQLITE_TRANSIENT);
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
        "SELECT game_id, system_key, system_name, display_name, canonical_name, core_name,"
        " asset_key, identity_key, version, language, region, content_stem, updated_at, source,"
        " content_path"
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

    if (!system_key.empty()) {
        if (auto scoped = try_kinds(system_key)) {
            return scoped;
        }
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
        " asset_key, identity_key, version, language, region, content_stem, updated_at, source,"
        " content_path"
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
            " asset_key, identity_key, version, language, region, content_stem, updated_at, source,"
            " content_path"
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

    std::optional<GameMetaRecord> existing = find_by_content_path(path_s);
    if (!existing) {
        existing = find_by_id(provisional_id);
        if (existing && !same_system(*existing)) {
            existing = std::nullopt;
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
        if (existing && !same_system(*existing)) {
            existing = std::nullopt;
        }
    }
    if (!existing) {
        existing = find_by_content_stem_column(stem);
        if (existing && !same_system(*existing)) {
            existing = std::nullopt;
        }
    }

    if (existing) {
        // Fold scanned provisional id + any other primary that claims this ROM stem
        // on the same system.
        fold_stale_into(provisional_id, *existing);
        if (!stem.empty()) {
            if (auto stem_claim = find_by_content_stem_column(stem);
                stem_claim && stem_claim->game_id != existing->game_id
                && same_system(*stem_claim)) {
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
        if (existing->content_stem.empty()) {
            existing->content_stem = stem;
        }
        if (!info.core_name.empty()) {
            existing->core_name = info.core_name;
        }
        if (!info.system_name.empty() && existing->system_name.empty()) {
            existing->system_name = info.system_name;
        }
        existing->updated_at = static_cast<std::int64_t>(info.updated_at);
        (void)upsert(*existing);
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
    row.content_stem = stem;
    row.content_path = path_s;
    row.updated_at = static_cast<std::int64_t>(info.updated_at);
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
    if (!row.version.empty() && row.version != "unknown") {
        doc["version"] = row.version;
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
    if (!row->version.empty() && row->version != "unknown") {
        if (auto base = match_hint(strip_trailing_version_label(row->display_name))) {
            if (base->version.empty() || base->version == "unknown") {
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
        " asset_key, identity_key, version, language, region, content_stem, updated_at, source,"
        " content_path"
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
    if (!row.canonical_name.empty()) {
        (void)upsert_alias(game_meta_alias::kCanonical, row.canonical_name, sys, row.game_id);
    }
    if (!row.content_stem.empty()) {
        (void)upsert_alias(game_meta_alias::kContentStem, row.content_stem, sys, row.game_id);
        if (sys == "switch" && looks_like_title_id(row.content_stem)) {
            (void)upsert_alias(game_meta_alias::kTitleId, row.content_stem, sys, row.game_id);
            (void)upsert_alias(game_meta_alias::kTitleId, row.content_stem, {}, row.game_id);
        }
        const auto stem_base = save_match_base_name(row.content_stem);
        if (!stem_base.empty() && stem_base != row.content_stem) {
            (void)upsert_alias(game_meta_alias::kContentStem, stem_base, sys, row.game_id);
        }
    }
    if (!row.display_name.empty()) {
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

} // namespace archstreamer
