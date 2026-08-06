#include "common/controls_db_pack.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <system_error>

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

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        const auto ca = static_cast<unsigned char>(a[i]);
        const auto cb = static_cast<unsigned char>(b[i]);
        if (std::tolower(ca) != std::tolower(cb)) {
            return false;
        }
    }
    return true;
}

std::filesystem::path make_temp_db_path(const char* prefix) {
    std::error_code ec;
    const auto dir = std::filesystem::temp_directory_path(ec);
    const auto base = ec ? std::filesystem::path(".") : dir;
    const auto name = std::string(prefix) + std::to_string(
        static_cast<unsigned long long>(
            std::chrono::high_resolution_clock::now().time_since_epoch().count())) +
        ".sqlite";
    return base / name;
}

bool open_db(const std::filesystem::path& path, sqlite3** out, int flags) {
    *out = nullptr;
    return sqlite3_open_v2(path.string().c_str(), out, flags, nullptr) == SQLITE_OK && *out != nullptr;
}

void close_db(sqlite3* db) {
    if (db != nullptr) {
        sqlite3_close(db);
    }
}

} // namespace

std::vector<std::uint8_t> export_controls_db_pack(
    std::string_view username,
    const std::vector<ControlsDbPackRow>& rows,
    std::string* error_out) {
    auto fail = [&](std::string message) -> std::vector<std::uint8_t> {
        if (error_out != nullptr) {
            *error_out = std::move(message);
        }
        return {};
    };
    if (username.empty()) {
        return fail("username required");
    }

    const auto path = make_temp_db_path("archstreamer_controls_pack_");
    std::error_code ec;
    std::filesystem::remove(path, ec);

    sqlite3* db = nullptr;
    if (!open_db(path, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE)) {
        return fail("failed to create controls pack database");
    }
    char* err = nullptr;
    if (sqlite3_exec(db, kCreateSql, nullptr, nullptr, &err) != SQLITE_OK) {
        if (err != nullptr) {
            sqlite3_free(err);
        }
        close_db(db);
        std::filesystem::remove(path, ec);
        return fail("failed to create user_controls table");
    }

    sqlite3_stmt* stmt = nullptr;
    const char* insert_sql =
        "INSERT INTO user_controls(username, kind, document_json, version, updated_at) "
        "VALUES (?, ?, ?, ?, ?);";
    if (sqlite3_prepare_v2(db, insert_sql, -1, &stmt, nullptr) != SQLITE_OK) {
        close_db(db);
        std::filesystem::remove(path, ec);
        return fail("failed to prepare pack insert");
    }

    for (const auto& row : rows) {
        if (row.username.empty() || row.kind.empty() || row.document_json.empty()) {
            continue;
        }
        if (!iequals(row.username, username)) {
            continue;
        }
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
        sqlite3_bind_text(stmt, 1, row.username.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, row.kind.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, row.document_json.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 4, row.version);
        sqlite3_bind_int64(stmt, 5, static_cast<sqlite3_int64>(row.updated_at));
        if (sqlite3_step(stmt) != SQLITE_DONE) {
            sqlite3_finalize(stmt);
            close_db(db);
            std::filesystem::remove(path, ec);
            return fail("failed to insert controls pack row");
        }
    }
    sqlite3_finalize(stmt);
    close_db(db);

    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size == 0 || size > kControlsDbPackMaxBytes) {
        std::filesystem::remove(path, ec);
        return fail(size > kControlsDbPackMaxBytes ? "controls pack too large" : "empty controls pack");
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::filesystem::remove(path, ec);
        return fail("failed to read controls pack");
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
    std::filesystem::remove(path, ec);
    if (!in) {
        return fail("failed to read controls pack");
    }
    return bytes;
}

std::optional<std::vector<ControlsDbPackRow>> import_controls_db_pack(
    const std::vector<std::uint8_t>& bytes,
    std::string_view expected_username,
    std::string* error_out) {
    auto fail = [&](std::string message) -> std::optional<std::vector<ControlsDbPackRow>> {
        if (error_out != nullptr) {
            *error_out = std::move(message);
        }
        return std::nullopt;
    };
    if (expected_username.empty()) {
        return fail("username required");
    }
    if (bytes.empty() || bytes.size() > kControlsDbPackMaxBytes) {
        return fail("invalid controls pack size");
    }

    const auto path = make_temp_db_path("archstreamer_controls_import_");
    std::error_code ec;
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out) {
            return fail("failed to write temp controls pack");
        }
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!out) {
            std::filesystem::remove(path, ec);
            return fail("failed to write temp controls pack");
        }
    }

    sqlite3* db = nullptr;
    if (!open_db(path, &db, SQLITE_OPEN_READONLY)) {
        std::filesystem::remove(path, ec);
        return fail("controls pack is not valid SQLite");
    }

    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "SELECT username, kind, document_json, version, updated_at FROM user_controls;";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        close_db(db);
        std::filesystem::remove(path, ec);
        return fail("controls pack missing user_controls table");
    }

    std::vector<ControlsDbPackRow> rows;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const auto* user = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        const auto* kind = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        const auto* json = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        if (user == nullptr || kind == nullptr || json == nullptr) {
            continue;
        }
        if (!iequals(user, expected_username)) {
            continue;
        }
        ControlsDbPackRow row;
        row.username = user;
        row.kind = kind;
        row.document_json = json;
        row.version = sqlite3_column_int(stmt, 3);
        row.updated_at = static_cast<std::int64_t>(sqlite3_column_int64(stmt, 4));
        rows.push_back(std::move(row));
    }
    sqlite3_finalize(stmt);
    close_db(db);
    std::filesystem::remove(path, ec);
    return rows;
}

} // namespace archstreamer
