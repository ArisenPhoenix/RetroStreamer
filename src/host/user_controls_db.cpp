#include "host/user_controls_db.hpp"

#include <sqlite3.h>

#include <cctype>
#include <ctime>
#include <fstream>
#include <system_error>
#include <unistd.h>

namespace archstreamer {
namespace {

constexpr const char* kCreateSql =
    "CREATE TABLE IF NOT EXISTS user_controls ("
    "  username TEXT NOT NULL,"
    "  kind TEXT NOT NULL DEFAULT 'button_map',"
    "  document_json TEXT NOT NULL,"
    "  version INTEGER NOT NULL DEFAULT 1,"
    "  updated_at INTEGER NOT NULL DEFAULT 0,"
    "  PRIMARY KEY (username, kind)"
    ");";

bool open_db(const std::filesystem::path& path, sqlite3** out, int flags) {
    *out = nullptr;
    return sqlite3_open_v2(path.string().c_str(), out, flags, nullptr) == SQLITE_OK && *out != nullptr;
}

void close_db(sqlite3* db) {
    if (db != nullptr) {
        sqlite3_close(db);
    }
}

bool ensure_schema(sqlite3* db) {
    char* err = nullptr;
    const int rc = sqlite3_exec(db, kCreateSql, nullptr, nullptr, &err);
    if (err != nullptr) {
        sqlite3_free(err);
    }
    return rc == SQLITE_OK;
}

std::int64_t now_epoch() {
    return static_cast<std::int64_t>(std::time(nullptr));
}

} // namespace

std::filesystem::path user_controls_db_path(const std::filesystem::path& user_directory) {
    return user_directory / std::string(kControlsDbFileName);
}

std::filesystem::path user_controls_db_path_for(
    const std::filesystem::path& save_root,
    std::string_view username) {
    return user_controls_db_path(save_root / std::string(username));
}

std::vector<std::uint8_t> read_user_controls_db_file(const std::filesystem::path& db_path) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(db_path, ec) || ec) {
        return {};
    }
    const auto size = std::filesystem::file_size(db_path, ec);
    if (ec || size == 0 || size > kControlsDbMaxBytes) {
        return {};
    }
    std::ifstream in(db_path, std::ios::binary);
    if (!in) {
        return {};
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
    if (!in) {
        return {};
    }
    return bytes;
}

bool write_user_controls_db_file(
    const std::filesystem::path& db_path,
    const std::vector<std::uint8_t>& bytes,
    std::string* error_out) {
    auto fail = [&](std::string message) {
        if (error_out != nullptr) {
            *error_out = std::move(message);
        }
        return false;
    };
    if (bytes.empty()) {
        return fail("empty controls database");
    }
    if (bytes.size() > kControlsDbMaxBytes) {
        return fail("controls database exceeds size limit");
    }

    // Write to temp then rename. Validate by opening the temp file.
    std::error_code ec;
    std::filesystem::create_directories(db_path.parent_path(), ec);
    const auto tmp = db_path.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            return fail("failed to write temp controls database");
        }
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!out) {
            return fail("failed to write temp controls database");
        }
    }

    sqlite3* db = nullptr;
    if (!open_db(tmp, &db, SQLITE_OPEN_READONLY) || !ensure_schema(db)) {
        // Readonly open won't create schema — just check we can query.
        close_db(db);
        db = nullptr;
        if (!open_db(tmp, &db, SQLITE_OPEN_READONLY)) {
            std::filesystem::remove(tmp, ec);
            return fail("controls database is not valid SQLite");
        }
    }
    // Verify user_controls exists.
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "SELECT name FROM sqlite_master WHERE type='table' AND name='user_controls' LIMIT 1;";
    bool has_table = false;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        has_table = sqlite3_step(stmt) == SQLITE_ROW;
        sqlite3_finalize(stmt);
    }
    close_db(db);
    if (!has_table) {
        std::filesystem::remove(tmp, ec);
        return fail("controls database missing user_controls table");
    }

    std::filesystem::rename(tmp, db_path, ec);
    if (ec) {
        std::filesystem::remove(db_path, ec);
        std::filesystem::rename(tmp, db_path, ec);
        if (ec) {
            std::filesystem::remove(tmp, ec);
            return fail("failed to install controls database");
        }
    }
    return true;
}

bool upsert_user_controls_row(
    const std::filesystem::path& db_path,
    const UserControlsRow& row) {
    if (row.username.empty() || row.kind.empty() || row.document_json.empty()) {
        return false;
    }
    std::error_code ec;
    std::filesystem::create_directories(db_path.parent_path(), ec);
    sqlite3* db = nullptr;
    if (!open_db(db_path, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE)) {
        close_db(db);
        return false;
    }
    if (!ensure_schema(db)) {
        close_db(db);
        return false;
    }
    const auto updated = row.updated_at > 0 ? row.updated_at : now_epoch();
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "INSERT INTO user_controls(username, kind, document_json, version, updated_at) "
        "VALUES(?,?,?,?,?) "
        "ON CONFLICT(username, kind) DO UPDATE SET "
        "  document_json=excluded.document_json,"
        "  version=excluded.version,"
        "  updated_at=excluded.updated_at;";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        close_db(db);
        return false;
    }
    sqlite3_bind_text(stmt, 1, row.username.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, row.kind.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, row.document_json.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 4, row.version);
    sqlite3_bind_int64(stmt, 5, updated);
    const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    close_db(db);
    return ok;
}

std::optional<UserControlsRow> find_user_controls_row(
    const std::filesystem::path& db_path,
    std::string_view username,
    std::string_view kind) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(db_path, ec) || ec) {
        return std::nullopt;
    }
    sqlite3* db = nullptr;
    if (!open_db(db_path, &db, SQLITE_OPEN_READONLY)) {
        close_db(db);
        return std::nullopt;
    }
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "SELECT username, kind, document_json, version, updated_at "
        "FROM user_controls WHERE username=? AND kind=? LIMIT 1;";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        close_db(db);
        return std::nullopt;
    }
    const auto user_s = std::string(username);
    const auto kind_s = std::string(kind);
    sqlite3_bind_text(stmt, 1, user_s.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, kind_s.c_str(), -1, SQLITE_TRANSIENT);
    std::optional<UserControlsRow> out;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        UserControlsRow row;
        row.username = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        row.kind = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        row.document_json = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        row.version = sqlite3_column_int(stmt, 3);
        row.updated_at = sqlite3_column_int64(stmt, 4);
        out = std::move(row);
    }
    sqlite3_finalize(stmt);
    close_db(db);
    return out;
}

bool validate_controls_db_pack(
    const std::vector<std::uint8_t>& bytes,
    std::string_view expected_username,
    std::string* error_out) {
    auto fail = [&](std::string message) {
        if (error_out != nullptr) {
            *error_out = std::move(message);
        }
        return false;
    };
    if (bytes.empty()) {
        return fail("empty controls database");
    }
    if (bytes.size() > kControlsDbMaxBytes) {
        return fail("controls database exceeds size limit");
    }

    // sqlite3_open needs a file path — write a temp.
    char tmpl[] = "/tmp/archstreamer_controls_XXXXXX";
    const int fd = ::mkstemp(tmpl);
    if (fd < 0) {
        return fail("failed to create temp file for validation");
    }
    const std::string tmp_path = tmpl;
    const auto written = ::write(fd, bytes.data(), bytes.size());
    ::close(fd);
    if (written != static_cast<ssize_t>(bytes.size())) {
        std::filesystem::remove(tmp_path);
        return fail("failed to stage controls database");
    }

    sqlite3* db = nullptr;
    if (!open_db(tmp_path, &db, SQLITE_OPEN_READONLY)) {
        close_db(db);
        std::filesystem::remove(tmp_path);
        return fail("controls database is not valid SQLite");
    }

    sqlite3_stmt* stmt = nullptr;
    const char* check =
        "SELECT name FROM sqlite_master WHERE type='table' AND name='user_controls' LIMIT 1;";
    bool has_table = false;
    if (sqlite3_prepare_v2(db, check, -1, &stmt, nullptr) == SQLITE_OK) {
        has_table = sqlite3_step(stmt) == SQLITE_ROW;
        sqlite3_finalize(stmt);
    }
    if (!has_table) {
        close_db(db);
        std::filesystem::remove(tmp_path);
        return fail("controls database missing user_controls table");
    }

    if (!expected_username.empty()) {
        const char* sql = "SELECT DISTINCT username FROM user_controls;";
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
                if (text == nullptr) {
                    sqlite3_finalize(stmt);
                    close_db(db);
                    std::filesystem::remove(tmp_path);
                    return fail("controls database username mismatch");
                }
                // Identity already comes from the authenticated socket; only ensure
                // the pack rows belong to that profile (case-insensitive).
                std::string row(text);
                std::string want(expected_username);
                for (char& ch : row) {
                    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                }
                for (char& ch : want) {
                    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                }
                if (row != want) {
                    sqlite3_finalize(stmt);
                    close_db(db);
                    std::filesystem::remove(tmp_path);
                    return fail("controls database username mismatch");
                }
            }
            sqlite3_finalize(stmt);
        }
    }

    close_db(db);
    std::filesystem::remove(tmp_path);
    return true;
}

} // namespace archstreamer
