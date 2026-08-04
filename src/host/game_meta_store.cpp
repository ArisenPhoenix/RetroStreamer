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
        "  asset_key, identity_key, version, language, region, content_stem, updated_at, source"
        ") VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?)"
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
        "  source=excluded.source;";
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
        " asset_key, identity_key, version, language, region, content_stem, updated_at, source"
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
        GameMetaRecord row;
        row.game_id = game.id;
        row.system_key = game.system_key;
        row.system_name = game.system_name;
        row.display_name = game.display_name;
        row.canonical_name = game.canonical_name;
        row.core_name = game.core_name;
        row.asset_key = game.asset_key;
        row.identity_key = game.identity_key;
        row.version = game.version;
        row.language = game.language;
        row.region = game.region;
        row.updated_at = static_cast<std::int64_t>(game.updated_at);
        row.source = std::string(game_meta_source::kCatalog);

        std::vector<std::pair<std::string, std::string>> extras;
        if (const auto hosted = catalog.find_hosted(game.id); hosted.has_value()) {
            const auto& path = hosted->get().content_path;
            row.content_stem = to_lower_copy(path.stem().string());
            read_optional_meta_aliases(path, row, extras);
        }

        if (!upsert(row)) {
            continue;
        }

        const auto& sys = row.system_key;
        (void)upsert_alias(game_meta_alias::kCatalogId, row.game_id, {}, row.game_id);
        (void)upsert_alias(game_meta_alias::kCatalogId, row.game_id, sys, row.game_id);
        if (!row.canonical_name.empty()) {
            (void)upsert_alias(game_meta_alias::kCanonical, row.canonical_name, sys, row.game_id);
            (void)upsert_alias(game_meta_alias::kCanonical, row.canonical_name, {}, row.game_id);
        }
        if (!row.content_stem.empty()) {
            (void)upsert_alias(game_meta_alias::kContentStem, row.content_stem, sys, row.game_id);
            (void)upsert_alias(game_meta_alias::kContentStem, row.content_stem, {}, row.game_id);
            if (sys == "switch" && looks_like_title_id(row.content_stem)) {
                (void)upsert_alias(game_meta_alias::kTitleId, row.content_stem, sys, row.game_id);
                (void)upsert_alias(game_meta_alias::kTitleId, row.content_stem, {}, row.game_id);
            }
        }
        if (!row.display_name.empty()) {
            (void)upsert_alias(game_meta_alias::kDisplayName, row.display_name, sys, row.game_id);
            (void)upsert_alias(game_meta_alias::kDisplayName, row.display_name, {}, row.game_id);
        }
        for (const auto& [kind, value] : extras) {
            (void)upsert_alias(kind, value, sys, row.game_id);
            (void)upsert_alias(kind, value, {}, row.game_id);
        }
    }
    (void)exec("COMMIT;");
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
        }
        if (canonical != nullptr && *canonical != '\0') {
            hints.by_stem[to_lower_copy(canonical)] = {system_s, display_s};
        }
        hints.by_stem[to_lower_copy(display_s)] = {system_s, display_s};
    }
    sqlite3_finalize(stmt);
    return hints;
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
