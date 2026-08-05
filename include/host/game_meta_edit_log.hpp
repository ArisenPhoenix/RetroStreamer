#pragma once

#include "host/game_meta_store.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace archstreamer {

/** One Catalog mutation snapshot (before → after) for audit / rollback. */
struct GameMetaEditRecord {
    std::int64_t edit_id = 0;
    std::int64_t edited_at = 0;
    /** "edit", "normalize", or "rollback". */
    std::string op;
    std::string old_game_id;
    std::string new_game_id;
    GameMetaRecord before;
    GameMetaRecord after;
    std::vector<std::string> effects;
    std::string note;
};

/**
 * Separate SQLite DB of Catalog edits (sibling of games.sqlite).
 * Soft-fail: a missing/unwritable log must not block Catalog ops.
 */
class GameMetaEditLog {
public:
    static std::filesystem::path default_path();

    explicit GameMetaEditLog(std::filesystem::path db_path = default_path());
    ~GameMetaEditLog();

    GameMetaEditLog(const GameMetaEditLog&) = delete;
    GameMetaEditLog& operator=(const GameMetaEditLog&) = delete;
    GameMetaEditLog(GameMetaEditLog&&) noexcept;
    GameMetaEditLog& operator=(GameMetaEditLog&&) noexcept;

    bool ready() const { return db_ != nullptr; }
    const std::filesystem::path& path() const { return db_path_; }

    /** Append a successful mutation. Returns new edit_id, or 0 on failure. */
    std::int64_t record(
        std::string_view op,
        const GameMetaRecord& before,
        const GameMetaRecord& after,
        const std::vector<std::string>& effects = {},
        std::string_view note = {});

    std::vector<GameMetaEditRecord> list_edits(std::size_t limit = 500) const;
    std::optional<GameMetaEditRecord> find_edit(std::int64_t edit_id) const;

private:
    bool open();
    bool ensure_schema();
    bool exec(const char* sql);

    std::filesystem::path db_path_;
    sqlite3* db_ = nullptr;
};

std::string game_meta_record_to_json(const GameMetaRecord& row);
std::optional<GameMetaRecord> game_meta_record_from_json(std::string_view json);

} // namespace archstreamer
