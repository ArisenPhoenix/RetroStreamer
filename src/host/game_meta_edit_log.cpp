#include "host/game_meta_edit_log.hpp"

#include "common/platform/paths.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <iostream>
#include <utility>

#include <sqlite3.h>

namespace archstreamer {
namespace {

nlohmann::json record_to_json_obj(const GameMetaRecord& row) {
    return nlohmann::json{
        {"game_id", row.game_id},
        {"system_key", row.system_key},
        {"system_name", row.system_name},
        {"display_name", row.display_name},
        {"canonical_name", row.canonical_name},
        {"core_name", row.core_name},
        {"asset_key", row.asset_key},
        {"identity_key", row.identity_key},
        {"version", row.version},
        {"language", row.language},
        {"region", row.region},
        {"content_stem", row.content_stem},
        {"content_path", row.content_path},
        {"created_at", row.created_at},
        {"updated_at", row.updated_at},
        {"source", row.source},
    };
}

GameMetaRecord record_from_json_obj(const nlohmann::json& j) {
    GameMetaRecord row;
    row.game_id = j.value("game_id", "");
    row.system_key = j.value("system_key", "");
    row.system_name = j.value("system_name", "");
    row.display_name = j.value("display_name", "");
    row.canonical_name = j.value("canonical_name", "");
    row.core_name = j.value("core_name", "");
    row.asset_key = j.value("asset_key", "");
    row.identity_key = j.value("identity_key", "");
    row.version = j.value("version", "");
    row.language = j.value("language", "");
    row.region = j.value("region", "");
    row.content_stem = j.value("content_stem", "");
    row.content_path = j.value("content_path", "");
    row.created_at = j.value("created_at", static_cast<std::int64_t>(0));
    row.updated_at = j.value("updated_at", static_cast<std::int64_t>(0));
    row.source = j.value("source", std::string(game_meta_source::kCatalog));
    return row;
}

GameMetaEditRecord row_from_stmt(sqlite3_stmt* stmt) {
    GameMetaEditRecord rec;
    rec.edit_id = sqlite3_column_int64(stmt, 0);
    rec.edited_at = sqlite3_column_int64(stmt, 1);
    rec.op = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
    rec.old_game_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
    rec.new_game_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
    const auto* before_c = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
    const auto* after_c = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
    const auto* effects_c = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 7));
    const auto* note_c = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 8));
    if (before_c) {
        if (auto parsed = game_meta_record_from_json(before_c)) {
            rec.before = *parsed;
        }
    }
    if (after_c) {
        if (auto parsed = game_meta_record_from_json(after_c)) {
            rec.after = *parsed;
        }
    }
    if (effects_c) {
        try {
            const auto arr = nlohmann::json::parse(effects_c);
            if (arr.is_array()) {
                for (const auto& item : arr) {
                    if (item.is_string()) {
                        rec.effects.push_back(item.get<std::string>());
                    }
                }
            }
        } catch (...) {
        }
    }
    if (note_c) {
        rec.note = note_c;
    }
    return rec;
}

} // namespace

std::string game_meta_record_to_json(const GameMetaRecord& row) {
    return record_to_json_obj(row).dump();
}

std::optional<GameMetaRecord> game_meta_record_from_json(std::string_view json) {
    try {
        return record_from_json_obj(nlohmann::json::parse(json));
    } catch (...) {
        return std::nullopt;
    }
}

std::filesystem::path GameMetaEditLog::default_path() {
    const auto home = user_home_directory();
    if (!home.empty()) {
        return std::filesystem::path(home) / ".local" / "share" / "archstreamer" / "meta"
            / "game_meta_edits.sqlite";
    }
    return std::filesystem::current_path() / "archstreamer-meta" / "game_meta_edits.sqlite";
}

GameMetaEditLog::GameMetaEditLog(std::filesystem::path db_path)
    : db_path_(std::move(db_path)) {
    (void)open();
}

GameMetaEditLog::~GameMetaEditLog() {
    if (db_ != nullptr) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

GameMetaEditLog::GameMetaEditLog(GameMetaEditLog&& other) noexcept
    : db_path_(std::move(other.db_path_))
    , db_(other.db_) {
    other.db_ = nullptr;
}

GameMetaEditLog& GameMetaEditLog::operator=(GameMetaEditLog&& other) noexcept {
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

bool GameMetaEditLog::exec(const char* sql) {
    char* err = nullptr;
    const int rc = sqlite3_exec(db_, sql, nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        std::cerr << "game_meta_edits: SQL error: " << (err ? err : sqlite3_errmsg(db_)) << '\n';
        sqlite3_free(err);
        return false;
    }
    return true;
}

bool GameMetaEditLog::ensure_schema() {
    return exec(
        "CREATE TABLE IF NOT EXISTS game_meta_edits ("
        "  edit_id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  edited_at INTEGER NOT NULL,"
        "  op TEXT NOT NULL,"
        "  old_game_id TEXT NOT NULL,"
        "  new_game_id TEXT NOT NULL,"
        "  before_json TEXT NOT NULL,"
        "  after_json TEXT NOT NULL,"
        "  effects_json TEXT NOT NULL DEFAULT '[]',"
        "  note TEXT NOT NULL DEFAULT ''"
        ");");
}

bool GameMetaEditLog::open() {
    if (db_ != nullptr) {
        return true;
    }
    std::error_code ec;
    std::filesystem::create_directories(db_path_.parent_path(), ec);
    if (sqlite3_open(db_path_.string().c_str(), &db_) != SQLITE_OK) {
        std::cerr << "game_meta_edits: open failed: " << db_path_ << '\n';
        if (db_ != nullptr) {
            sqlite3_close(db_);
            db_ = nullptr;
        }
        return false;
    }
    if (!ensure_schema()) {
        sqlite3_close(db_);
        db_ = nullptr;
        return false;
    }
    return true;
}

std::int64_t GameMetaEditLog::record(
    std::string_view op,
    const GameMetaRecord& before,
    const GameMetaRecord& after,
    const std::vector<std::string>& effects,
    std::string_view note) {
    if (!ready()) {
        return 0;
    }
    const auto now = static_cast<std::int64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    nlohmann::json effects_json = nlohmann::json::array();
    for (const auto& line : effects) {
        effects_json.push_back(line);
    }
    const auto before_s = game_meta_record_to_json(before);
    const auto after_s = game_meta_record_to_json(after);
    const auto effects_s = effects_json.dump();
    const std::string op_s(op);
    const std::string note_s(note);

    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "INSERT INTO game_meta_edits"
        " (edited_at, op, old_game_id, new_game_id, before_json, after_json, effects_json, note)"
        " VALUES (?, ?, ?, ?, ?, ?, ?, ?);";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return 0;
    }
    sqlite3_bind_int64(stmt, 1, now);
    sqlite3_bind_text(stmt, 2, op_s.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, before.game_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, after.game_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, before_s.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, after_s.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 7, effects_s.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 8, note_s.c_str(), -1, SQLITE_TRANSIENT);
    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        return 0;
    }
    return static_cast<std::int64_t>(sqlite3_last_insert_rowid(db_));
}

std::vector<GameMetaEditRecord> GameMetaEditLog::list_edits(std::size_t limit) const {
    std::vector<GameMetaEditRecord> out;
    if (!ready()) {
        return out;
    }
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "SELECT edit_id, edited_at, op, old_game_id, new_game_id,"
        " before_json, after_json, effects_json, note"
        " FROM game_meta_edits ORDER BY edit_id DESC LIMIT ?;";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return out;
    }
    sqlite3_bind_int64(stmt, 1, static_cast<std::int64_t>(limit));
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        out.push_back(row_from_stmt(stmt));
    }
    sqlite3_finalize(stmt);
    return out;
}

std::optional<GameMetaEditRecord> GameMetaEditLog::find_edit(std::int64_t edit_id) const {
    if (!ready()) {
        return std::nullopt;
    }
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "SELECT edit_id, edited_at, op, old_game_id, new_game_id,"
        " before_json, after_json, effects_json, note"
        " FROM game_meta_edits WHERE edit_id = ?;";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return std::nullopt;
    }
    sqlite3_bind_int64(stmt, 1, edit_id);
    std::optional<GameMetaEditRecord> out;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        out = row_from_stmt(stmt);
    }
    sqlite3_finalize(stmt);
    return out;
}

} // namespace archstreamer
