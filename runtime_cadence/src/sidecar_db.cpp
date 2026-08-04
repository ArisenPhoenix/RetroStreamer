#include "archstreamer/runtime_cadence/sidecar_db.hpp"

#include "archstreamer/runtime_cadence/protocol.hpp"

#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <cctype>
#include <iostream>
#include <sstream>
#include <system_error>

namespace archstreamer::cadence {

SidecarDb::SidecarDb(std::filesystem::path db_path)
    : db_path_(std::move(db_path)) {}

SidecarDb::~SidecarDb() {
    if (db_ != nullptr) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

bool SidecarDb::exec(const char* sql) {
    char* err = nullptr;
    const int rc = sqlite3_exec(db_, sql, nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        std::cerr << "cadence(db): SQL error: " << (err ? err : sqlite3_errmsg(db_)) << '\n';
        sqlite3_free(err);
        return false;
    }
    return true;
}

bool SidecarDb::exec_quiet(const char* sql) {
    char* err = nullptr;
    const int rc = sqlite3_exec(db_, sql, nullptr, nullptr, &err);
    sqlite3_free(err);
    return rc == SQLITE_OK;
}

bool SidecarDb::open() {
    std::error_code ec;
    std::filesystem::create_directories(db_path_.parent_path(), ec);
    if (sqlite3_open(db_path_.string().c_str(), &db_) != SQLITE_OK) {
        std::cerr << "cadence(db): open failed: " << sqlite3_errmsg(db_) << '\n';
        return false;
    }
    if (!exec("PRAGMA journal_mode=WAL;")) {
        return false;
    }
    if (!exec("PRAGMA synchronous=NORMAL;")) {
        return false;
    }
    return ensure_schema();
}

bool SidecarDb::ensure_schema() {
    if (!exec(
            "CREATE TABLE IF NOT EXISTS users ("
            "  username TEXT PRIMARY KEY NOT NULL,"
            "  display_name TEXT NOT NULL DEFAULT '',"
            "  password_hash TEXT NOT NULL DEFAULT '',"
            "  must_change INTEGER NOT NULL DEFAULT 0,"
            "  profile_path TEXT NOT NULL DEFAULT '',"
            "  save_root TEXT NOT NULL DEFAULT '',"
            "  created_at INTEGER NOT NULL DEFAULT 0,"
            "  updated_at INTEGER NOT NULL DEFAULT 0"
            ");")) {
        return false;
    }
    (void)exec_quiet("ALTER TABLE users ADD COLUMN password_hash TEXT NOT NULL DEFAULT '';");
    (void)exec_quiet("ALTER TABLE users ADD COLUMN must_change INTEGER NOT NULL DEFAULT 0;");
    (void)exec_quiet("ALTER TABLE users ADD COLUMN created_at INTEGER NOT NULL DEFAULT 0;");
    (void)exec_quiet("ALTER TABLE users ADD COLUMN profile_path TEXT NOT NULL DEFAULT '';");
    (void)exec_quiet("ALTER TABLE users ADD COLUMN save_root TEXT NOT NULL DEFAULT '';");

    if (!exec(
            "CREATE TABLE IF NOT EXISTS user_controls ("
            "  username TEXT NOT NULL,"
            "  kind TEXT NOT NULL DEFAULT 'button_map',"
            "  document_json TEXT NOT NULL,"
            "  version INTEGER NOT NULL DEFAULT 1,"
            "  updated_at INTEGER NOT NULL DEFAULT 0,"
            "  PRIMARY KEY (username, kind)"
            ");")) {
        return false;
    }

    if (!exec(
            "CREATE TABLE IF NOT EXISTS sessions ("
            "  session_id TEXT PRIMARY KEY NOT NULL,"
            "  host_id TEXT NOT NULL DEFAULT '',"
            "  slot INTEGER NOT NULL DEFAULT -1,"
            "  username TEXT NOT NULL DEFAULT '',"
            "  game_key TEXT NOT NULL DEFAULT '',"
            "  system_key TEXT NOT NULL DEFAULT '',"
            "  mode TEXT NOT NULL DEFAULT '',"
            "  started_at INTEGER NOT NULL DEFAULT 0,"
            "  ended_at INTEGER NOT NULL DEFAULT 0,"
            "  end_reason TEXT NOT NULL DEFAULT ''"
            ");")) {
        return false;
    }

    if (!exec(
            "CREATE TABLE IF NOT EXISTS resource_claims ("
            "  resource_type TEXT NOT NULL,"
            "  resource_name TEXT NOT NULL,"
            "  session_id TEXT NOT NULL,"
            "  host_id TEXT NOT NULL DEFAULT '',"
            "  slot INTEGER NOT NULL DEFAULT -1,"
            "  claimed_at INTEGER NOT NULL DEFAULT 0,"
            "  released_at INTEGER NOT NULL DEFAULT 0,"
            "  detail TEXT NOT NULL DEFAULT '',"
            "  PRIMARY KEY (resource_type, resource_name)"
            ");")) {
        return false;
    }
    return true;
}

std::string SidecarDb::events_table_name(const std::string& day) {
    // day is YYYY-MM-DD → events_YYYY_MM_DD
    std::string name = "events_";
    for (const char c : day) {
        name.push_back(c == '-' ? '_' : c);
    }
    return name;
}

bool SidecarDb::ensure_events_table(const std::string& day) {
    const auto table = events_table_name(day);
    // Validate table name chars (defense in depth).
    for (const char c : table) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) {
            return false;
        }
    }
    const std::string sql =
        "CREATE TABLE IF NOT EXISTS " + table + " ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  ts INTEGER NOT NULL,"
        "  kind TEXT NOT NULL,"
        "  host_id TEXT NOT NULL DEFAULT '',"
        "  slot INTEGER NOT NULL DEFAULT -1,"
        "  username TEXT NOT NULL DEFAULT '',"
        "  game_key TEXT NOT NULL DEFAULT '',"
        "  detail TEXT NOT NULL DEFAULT '',"
        "  session_id TEXT NOT NULL DEFAULT ''"
        ");";
    if (!exec(sql.c_str())) {
        return false;
    }
    (void)exec_quiet(
        ("ALTER TABLE " + table + " ADD COLUMN session_id TEXT NOT NULL DEFAULT '';").c_str());
    return true;
}

bool SidecarDb::upsert_user(const UserRecord& user) {
    if (user.username.empty() || db_ == nullptr) {
        return false;
    }
    std::lock_guard lock(mutex_);
    UserRecord stored = user;
    if (stored.updated_at <= 0) {
        stored.updated_at = now_epoch_seconds();
    }
    if (stored.created_at <= 0) {
        stored.created_at = stored.updated_at;
    }
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "INSERT INTO users(username, display_name, password_hash, must_change, profile_path, save_root, "
        "created_at, updated_at) VALUES(?,?,?,?,?,?,?,?) "
        "ON CONFLICT(username) DO UPDATE SET "
        "display_name=excluded.display_name, "
        "password_hash=CASE WHEN excluded.password_hash='' THEN users.password_hash ELSE excluded.password_hash END, "
        "must_change=CASE WHEN excluded.password_hash='' THEN users.must_change ELSE excluded.must_change END, "
        "profile_path=CASE WHEN excluded.profile_path='' THEN users.profile_path ELSE excluded.profile_path END, "
        "save_root=CASE WHEN excluded.save_root='' THEN users.save_root ELSE excluded.save_root END, "
        "created_at=CASE WHEN users.created_at=0 THEN excluded.created_at ELSE users.created_at END, "
        "updated_at=excluded.updated_at;";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_text(stmt, 1, stored.username.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, stored.display_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, stored.password_hash.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 4, stored.must_change ? 1 : 0);
    sqlite3_bind_text(stmt, 5, stored.profile_path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, stored.save_root.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 7, stored.created_at);
    sqlite3_bind_int64(stmt, 8, stored.updated_at);
    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

std::optional<UserRecord> SidecarDb::find_user(const std::string& username) {
    if (username.empty() || db_ == nullptr) {
        return std::nullopt;
    }
    std::lock_guard lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "SELECT username, display_name, password_hash, must_change, profile_path, save_root, "
        "created_at, updated_at FROM users WHERE username=? LIMIT 1;";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return std::nullopt;
    }
    sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_TRANSIENT);
    std::optional<UserRecord> out;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        UserRecord user;
        user.username = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        user.display_name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        if (const auto* hash = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
            hash != nullptr) {
            user.password_hash = hash;
        }
        user.must_change = sqlite3_column_int(stmt, 3) != 0;
        if (const auto* path = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
            path != nullptr) {
            user.profile_path = path;
        }
        if (const auto* root = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
            root != nullptr) {
            user.save_root = root;
        }
        user.created_at = sqlite3_column_int64(stmt, 6);
        user.updated_at = sqlite3_column_int64(stmt, 7);
        out = std::move(user);
    }
    sqlite3_finalize(stmt);
    return out;
}

bool SidecarDb::delete_user(const std::string& username) {
    if (username.empty() || db_ == nullptr) {
        return false;
    }
    std::lock_guard lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "DELETE FROM users WHERE username=?;";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_TRANSIENT);
    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

std::vector<UserRecord> SidecarDb::list_users() {
    if (db_ == nullptr) {
        return {};
    }
    std::lock_guard lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "SELECT username, display_name, password_hash, must_change, profile_path, save_root, "
        "created_at, updated_at FROM users ORDER BY username COLLATE NOCASE;";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return {};
    }
    std::vector<UserRecord> out;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        UserRecord user;
        user.username = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        user.display_name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        if (const auto* hash = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
            hash != nullptr) {
            user.password_hash = hash;
        }
        user.must_change = sqlite3_column_int(stmt, 3) != 0;
        if (const auto* path = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
            path != nullptr) {
            user.profile_path = path;
        }
        if (const auto* root = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
            root != nullptr) {
            user.save_root = root;
        }
        user.created_at = sqlite3_column_int64(stmt, 6);
        user.updated_at = sqlite3_column_int64(stmt, 7);
        out.push_back(std::move(user));
    }
    sqlite3_finalize(stmt);
    return out;
}

bool SidecarDb::upsert_controls(const ControlsRecord& controls) {
    if (controls.username.empty() || controls.document_json.empty() || db_ == nullptr) {
        return false;
    }
    std::lock_guard lock(mutex_);
    ControlsRecord stored = controls;
    if (stored.kind.empty()) {
        stored.kind = std::string(kControlsKindButtonMap);
    }
    if (stored.updated_at <= 0) {
        stored.updated_at = now_epoch_seconds();
    }
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "INSERT INTO user_controls(username, kind, document_json, version, updated_at) "
        "VALUES(?,?,?,?,?) "
        "ON CONFLICT(username, kind) DO UPDATE SET "
        "document_json=excluded.document_json, version=excluded.version, "
        "updated_at=excluded.updated_at;";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_text(stmt, 1, stored.username.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, stored.kind.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, stored.document_json.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 4, stored.version);
    sqlite3_bind_int64(stmt, 5, stored.updated_at);
    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

std::optional<ControlsRecord> SidecarDb::find_controls(
    const std::string& username,
    const std::string& kind) {
    if (username.empty() || db_ == nullptr) {
        return std::nullopt;
    }
    const auto use_kind = kind.empty() ? std::string(kControlsKindButtonMap) : kind;
    std::lock_guard lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "SELECT username, kind, document_json, version, updated_at "
        "FROM user_controls WHERE username=? AND kind=? LIMIT 1;";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return std::nullopt;
    }
    sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, use_kind.c_str(), -1, SQLITE_TRANSIENT);
    std::optional<ControlsRecord> out;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        ControlsRecord controls;
        controls.username = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        controls.kind = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        controls.document_json = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        controls.version = sqlite3_column_int(stmt, 3);
        controls.updated_at = sqlite3_column_int64(stmt, 4);
        out = std::move(controls);
    }
    sqlite3_finalize(stmt);
    return out;
}

std::vector<ControlsRecord> SidecarDb::list_controls() {
    if (db_ == nullptr) {
        return {};
    }
    std::lock_guard lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "SELECT username, kind, document_json, version, updated_at "
        "FROM user_controls ORDER BY username COLLATE NOCASE, kind;";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return {};
    }
    std::vector<ControlsRecord> out;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        ControlsRecord controls;
        controls.username = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        controls.kind = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        controls.document_json = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        controls.version = sqlite3_column_int(stmt, 3);
        controls.updated_at = sqlite3_column_int64(stmt, 4);
        out.push_back(std::move(controls));
    }
    sqlite3_finalize(stmt);
    return out;
}

bool SidecarDb::upsert_session(const SessionRecord& session) {
    if (session.session_id.empty() || db_ == nullptr) {
        return false;
    }
    std::lock_guard lock(mutex_);
    SessionRecord stored = session;
    if (stored.started_at <= 0) {
        stored.started_at = now_epoch_seconds();
    }
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "INSERT INTO sessions(session_id, host_id, slot, username, game_key, system_key, mode, "
        "started_at, ended_at, end_reason) VALUES(?,?,?,?,?,?,?,?,?,?) "
        "ON CONFLICT(session_id) DO UPDATE SET "
        "host_id=excluded.host_id, slot=excluded.slot, username=excluded.username, "
        "game_key=excluded.game_key, system_key=excluded.system_key, mode=excluded.mode, "
        "started_at=excluded.started_at, ended_at=excluded.ended_at, end_reason=excluded.end_reason;";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_text(stmt, 1, stored.session_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, stored.host_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, stored.slot);
    sqlite3_bind_text(stmt, 4, stored.username.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, stored.game_key.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, stored.system_key.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 7, stored.mode.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 8, stored.started_at);
    sqlite3_bind_int64(stmt, 9, stored.ended_at);
    sqlite3_bind_text(stmt, 10, stored.end_reason.c_str(), -1, SQLITE_TRANSIENT);
    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

bool SidecarDb::end_session(const std::string& session_id, const std::string& end_reason) {
    if (session_id.empty() || db_ == nullptr) {
        return false;
    }
    std::lock_guard lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "UPDATE sessions SET ended_at=?, end_reason=? WHERE session_id=? AND ended_at=0;";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_int64(stmt, 1, now_epoch_seconds());
    sqlite3_bind_text(stmt, 2, end_reason.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, session_id.c_str(), -1, SQLITE_TRANSIENT);
    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

std::optional<SessionRecord> SidecarDb::find_session(const std::string& session_id) {
    if (session_id.empty() || db_ == nullptr) {
        return std::nullopt;
    }
    std::lock_guard lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "SELECT session_id, host_id, slot, username, game_key, system_key, mode, "
        "started_at, ended_at, end_reason FROM sessions WHERE session_id=? LIMIT 1;";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return std::nullopt;
    }
    sqlite3_bind_text(stmt, 1, session_id.c_str(), -1, SQLITE_TRANSIENT);
    std::optional<SessionRecord> out;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        SessionRecord session;
        session.session_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        session.host_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        session.slot = sqlite3_column_int(stmt, 2);
        session.username = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        session.game_key = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        session.system_key = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
        session.mode = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
        session.started_at = sqlite3_column_int64(stmt, 7);
        session.ended_at = sqlite3_column_int64(stmt, 8);
        session.end_reason = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 9));
        out = std::move(session);
    }
    sqlite3_finalize(stmt);
    return out;
}

std::vector<SessionRecord> SidecarDb::list_sessions(bool active_only) {
    if (db_ == nullptr) {
        return {};
    }
    std::lock_guard lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    const char* sql = active_only
        ? "SELECT session_id, host_id, slot, username, game_key, system_key, mode, "
          "started_at, ended_at, end_reason FROM sessions WHERE ended_at=0 "
          "ORDER BY started_at DESC;"
        : "SELECT session_id, host_id, slot, username, game_key, system_key, mode, "
          "started_at, ended_at, end_reason FROM sessions ORDER BY started_at DESC;";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return {};
    }
    std::vector<SessionRecord> out;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        SessionRecord session;
        session.session_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        session.host_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        session.slot = sqlite3_column_int(stmt, 2);
        session.username = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        session.game_key = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        session.system_key = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
        session.mode = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
        session.started_at = sqlite3_column_int64(stmt, 7);
        session.ended_at = sqlite3_column_int64(stmt, 8);
        session.end_reason = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 9));
        out.push_back(std::move(session));
    }
    sqlite3_finalize(stmt);
    return out;
}

bool SidecarDb::claim_resource(const ResourceClaim& claim) {
    if (claim.session_id.empty() || claim.resource_type.empty() ||
        claim.resource_name.empty() || db_ == nullptr) {
        return false;
    }
    std::lock_guard lock(mutex_);
    ResourceClaim stored = claim;
    if (stored.claimed_at <= 0) {
        stored.claimed_at = now_epoch_seconds();
    }
    stored.released_at = 0;
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "INSERT INTO resource_claims(resource_type, resource_name, session_id, host_id, slot, "
        "claimed_at, released_at, detail) VALUES(?,?,?,?,?,?,0,?) "
        "ON CONFLICT(resource_type, resource_name) DO UPDATE SET "
        "session_id=excluded.session_id, host_id=excluded.host_id, slot=excluded.slot, "
        "claimed_at=excluded.claimed_at, released_at=0, detail=excluded.detail;";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_text(stmt, 1, stored.resource_type.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, stored.resource_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, stored.session_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, stored.host_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 5, stored.slot);
    sqlite3_bind_int64(stmt, 6, stored.claimed_at);
    sqlite3_bind_text(stmt, 7, stored.detail.c_str(), -1, SQLITE_TRANSIENT);
    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

bool SidecarDb::release_resource(
    const std::string& session_id,
    const std::string& resource_type,
    const std::string& resource_name) {
    if (session_id.empty() || resource_type.empty() || resource_name.empty() || db_ == nullptr) {
        return false;
    }
    std::lock_guard lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "UPDATE resource_claims SET released_at=? "
        "WHERE resource_type=? AND resource_name=? AND session_id=? AND released_at=0;";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_int64(stmt, 1, now_epoch_seconds());
    sqlite3_bind_text(stmt, 2, resource_type.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, resource_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, session_id.c_str(), -1, SQLITE_TRANSIENT);
    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

bool SidecarDb::release_session_resources(const std::string& session_id) {
    if (session_id.empty() || db_ == nullptr) {
        return false;
    }
    std::lock_guard lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "UPDATE resource_claims SET released_at=? WHERE session_id=? AND released_at=0;";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_int64(stmt, 1, now_epoch_seconds());
    sqlite3_bind_text(stmt, 2, session_id.c_str(), -1, SQLITE_TRANSIENT);
    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

std::vector<ResourceClaim> SidecarDb::list_claims(bool held_only) {
    if (db_ == nullptr) {
        return {};
    }
    std::lock_guard lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    const char* sql = held_only
        ? "SELECT session_id, resource_type, resource_name, host_id, slot, claimed_at, "
          "released_at, detail FROM resource_claims WHERE released_at=0 "
          "ORDER BY resource_type, resource_name;"
        : "SELECT session_id, resource_type, resource_name, host_id, slot, claimed_at, "
          "released_at, detail FROM resource_claims "
          "ORDER BY resource_type, resource_name;";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return {};
    }
    std::vector<ResourceClaim> out;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        ResourceClaim claim;
        claim.session_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        claim.resource_type = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        claim.resource_name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        claim.host_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        claim.slot = sqlite3_column_int(stmt, 4);
        claim.claimed_at = sqlite3_column_int64(stmt, 5);
        claim.released_at = sqlite3_column_int64(stmt, 6);
        claim.detail = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 7));
        out.push_back(std::move(claim));
    }
    sqlite3_finalize(stmt);
    return out;
}

std::optional<ResourceClaim> SidecarDb::find_held_resource(
    const std::string& resource_type,
    const std::string& resource_name) {
    if (resource_type.empty() || resource_name.empty() || db_ == nullptr) {
        return std::nullopt;
    }
    std::lock_guard lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "SELECT session_id, resource_type, resource_name, host_id, slot, claimed_at, "
        "released_at, detail FROM resource_claims "
        "WHERE resource_type=? AND resource_name=? AND released_at=0 LIMIT 1;";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return std::nullopt;
    }
    sqlite3_bind_text(stmt, 1, resource_type.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, resource_name.c_str(), -1, SQLITE_TRANSIENT);
    std::optional<ResourceClaim> out;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        ResourceClaim claim;
        claim.session_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        claim.resource_type = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        claim.resource_name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        claim.host_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        claim.slot = sqlite3_column_int(stmt, 4);
        claim.claimed_at = sqlite3_column_int64(stmt, 5);
        claim.released_at = sqlite3_column_int64(stmt, 6);
        claim.detail = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 7));
        out = std::move(claim);
    }
    sqlite3_finalize(stmt);
    return out;
}

bool SidecarDb::record_event(const RuntimeEvent& event) {
    if (db_ == nullptr) {
        return false;
    }
    RuntimeEvent stored = event;
    if (stored.timestamp <= 0) {
        stored.timestamp = now_epoch_seconds();
    }
    const auto day = day_string_from_epoch(stored.timestamp);
    std::lock_guard lock(mutex_);
    if (!ensure_events_table(day)) {
        return false;
    }
    const auto table = events_table_name(day);
    const std::string sql =
        "INSERT INTO " + table +
        "(ts, kind, host_id, slot, username, game_key, detail, session_id) "
        "VALUES(?,?,?,?,?,?,?,?);";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_int64(stmt, 1, stored.timestamp);
    sqlite3_bind_text(stmt, 2, stored.kind.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, stored.host_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 4, stored.slot);
    sqlite3_bind_text(stmt, 5, stored.username.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, stored.game_key.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 7, stored.detail.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 8, stored.session_id.c_str(), -1, SQLITE_TRANSIENT);
    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

std::vector<RuntimeEvent> SidecarDb::recent_events(
    const std::string& day,
    std::size_t limit) {
    if (db_ == nullptr) {
        return {};
    }
    const auto use_day = day.empty() ? day_string_from_epoch(now_epoch_seconds()) : day;
    std::lock_guard lock(mutex_);
    if (!ensure_events_table(use_day)) {
        return {};
    }
    const auto table = events_table_name(use_day);
    const std::size_t cap = limit == 0 ? 100 : limit;
    std::ostringstream sql;
    sql << "SELECT ts, kind, host_id, slot, username, game_key, detail, session_id FROM " << table
        << " ORDER BY id DESC LIMIT " << cap << ";";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.str().c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        return {};
    }
    std::vector<RuntimeEvent> out;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        RuntimeEvent event;
        event.timestamp = sqlite3_column_int64(stmt, 0);
        event.kind = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        event.host_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        event.slot = sqlite3_column_int(stmt, 3);
        event.username = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        event.game_key = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
        event.detail = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
        if (const auto* sid = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 7));
            sid != nullptr) {
            event.session_id = sid;
        }
        out.push_back(std::move(event));
    }
    sqlite3_finalize(stmt);
    return out;
}

std::string SidecarDb::handle_request_json(const std::string& request_json) {
    nlohmann::json resp{{"ok", false}};
    try {
        const auto req = nlohmann::json::parse(request_json);
        const auto op = req.value("op", "");
        if (op == "ping") {
            resp["ok"] = true;
            resp["pong"] = true;
            return resp.dump();
        }
        if (op == "upsert_user") {
            const auto user = user_from_json(req);
            resp["ok"] = upsert_user(user);
            return resp.dump();
        }
        if (op == "find_user") {
            const auto username = req.value("username", "");
            if (const auto user = find_user(username); user.has_value()) {
                resp["ok"] = true;
                resp["user"] = user_to_json(*user);
            } else {
                resp["ok"] = true;
                resp["user"] = nullptr;
            }
            return resp.dump();
        }
        if (op == "delete_user") {
            resp["ok"] = delete_user(req.value("username", ""));
            return resp.dump();
        }
        if (op == "list_users") {
            auto users = list_users();
            nlohmann::json arr = nlohmann::json::array();
            for (const auto& u : users) {
                arr.push_back(user_to_json(u));
            }
            resp["ok"] = true;
            resp["users"] = std::move(arr);
            return resp.dump();
        }
        if (op == "upsert_controls") {
            resp["ok"] = upsert_controls(controls_from_json(req));
            return resp.dump();
        }
        if (op == "find_controls") {
            if (const auto controls = find_controls(
                    req.value("username", ""),
                    req.value("kind", std::string(kControlsKindButtonMap)));
                controls.has_value()) {
                resp["ok"] = true;
                resp["controls"] = controls_to_json(*controls);
            } else {
                resp["ok"] = true;
                resp["controls"] = nullptr;
            }
            return resp.dump();
        }
        if (op == "list_controls") {
            auto controls = list_controls();
            nlohmann::json arr = nlohmann::json::array();
            for (const auto& c : controls) {
                arr.push_back(controls_to_json(c));
            }
            resp["ok"] = true;
            resp["controls"] = std::move(arr);
            return resp.dump();
        }
        if (op == "upsert_session") {
            resp["ok"] = upsert_session(session_from_json(req));
            return resp.dump();
        }
        if (op == "end_session") {
            resp["ok"] = end_session(req.value("session_id", ""), req.value("end_reason", ""));
            return resp.dump();
        }
        if (op == "find_session") {
            if (const auto session = find_session(req.value("session_id", "")); session.has_value()) {
                resp["ok"] = true;
                resp["session"] = session_to_json(*session);
            } else {
                resp["ok"] = true;
                resp["session"] = nullptr;
            }
            return resp.dump();
        }
        if (op == "list_sessions") {
            auto sessions = list_sessions(req.value("active_only", true));
            nlohmann::json arr = nlohmann::json::array();
            for (const auto& s : sessions) {
                arr.push_back(session_to_json(s));
            }
            resp["ok"] = true;
            resp["sessions"] = std::move(arr);
            return resp.dump();
        }
        if (op == "claim_resource") {
            resp["ok"] = claim_resource(claim_from_json(req));
            return resp.dump();
        }
        if (op == "release_resource") {
            resp["ok"] = release_resource(
                req.value("session_id", ""),
                req.value("resource_type", ""),
                req.value("resource_name", ""));
            return resp.dump();
        }
        if (op == "release_session_resources") {
            resp["ok"] = release_session_resources(req.value("session_id", ""));
            return resp.dump();
        }
        if (op == "list_claims") {
            auto claims = list_claims(req.value("held_only", true));
            nlohmann::json arr = nlohmann::json::array();
            for (const auto& c : claims) {
                arr.push_back(claim_to_json(c));
            }
            resp["ok"] = true;
            resp["claims"] = std::move(arr);
            return resp.dump();
        }
        if (op == "find_held_resource") {
            if (const auto claim = find_held_resource(
                    req.value("resource_type", ""),
                    req.value("resource_name", ""));
                claim.has_value()) {
                resp["ok"] = true;
                resp["claim"] = claim_to_json(*claim);
            } else {
                resp["ok"] = true;
                resp["claim"] = nullptr;
            }
            return resp.dump();
        }
        if (op == "record_event") {
            resp["ok"] = record_event(event_from_json(req));
            return resp.dump();
        }
        if (op == "recent_events") {
            const auto day = req.value("day", "");
            const auto limit = req.value("limit", std::size_t{50});
            auto events = recent_events(day, limit);
            nlohmann::json arr = nlohmann::json::array();
            for (const auto& e : events) {
                arr.push_back(event_to_json(e));
            }
            resp["ok"] = true;
            resp["events"] = std::move(arr);
            return resp.dump();
        }
        resp["error"] = "unknown op";
    } catch (const nlohmann::json::exception& error) {
        resp["error"] = error.what();
    }
    return resp.dump();
}

} // namespace archstreamer::cadence
