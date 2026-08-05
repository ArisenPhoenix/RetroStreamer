#pragma once

#include "common/protocol.hpp"
#include "host/game_catalog.hpp"
#include "host/save_manager.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct sqlite3;

namespace archstreamer {

/** Alias kinds stored in game_aliases (pass-1 + reserved for pass-2). */
namespace game_meta_alias {
inline constexpr std::string_view kCatalogId = "catalog_id";
inline constexpr std::string_view kCanonical = "canonical";
inline constexpr std::string_view kContentStem = "content_stem";
inline constexpr std::string_view kDisplayName = "display_name";
inline constexpr std::string_view kTitleId = "title_id";
/** Reserved — PS2 product codes (SLUS/SCES/…). */
inline constexpr std::string_view kSerial = "serial";
/** Reserved — PS2 memcard save / product id extraction. */
inline constexpr std::string_view kPs2Product = "ps2_product";
} // namespace game_meta_alias

namespace game_meta_source {
inline constexpr std::string_view kCatalog = "catalog";
inline constexpr std::string_view kMetaJson = "meta_json";
inline constexpr std::string_view kPs2Memcard = "ps2_memcard";
} // namespace game_meta_source

/** One durable game metadata row (catalog GameInfo.id is the primary key). */
struct GameMetaRecord {
    std::string game_id;
    std::string system_key;
    std::string system_name;
    std::string display_name;
    std::string canonical_name;
    std::string core_name;
    std::string asset_key;
    std::string identity_key;
    std::string version;
    std::string language;
    std::string region;
    /** Absolute ROM/content path last seen for this game (empty until first scan). */
    std::string content_path;
    /**
     * Lowercased edge ROM stem for save matching — derived from
     * display_name + version via catalog_content_stem_for (not hand-edited).
     */
    std::string content_stem;
    /** Unix seconds — approximate add time from Meta directory birth/mtime. */
    std::int64_t created_at = 0;
    /** Unix seconds — last content/meta write or catalog edit. */
    std::int64_t updated_at = 0;
    std::string source = std::string(game_meta_source::kCatalog);
};

/** Per-user association with a catalog game (drives PS2 Users rows + Active). */
struct UserGameRecord {
    std::string username;
    std::string game_id;
    std::string system_key;
    std::int64_t last_played_at = 0;
};

/** Play modes / player limits from Meta JSON (modes.single/multi, min/max_players). */
struct GamePlayModesRecord {
    std::string game_id;
    bool supports_singleplayer = true;
    bool supports_multiplayer = true;
    std::uint8_t min_players = 1;
    std::uint8_t max_players = 2;
    std::int64_t updated_at = 0;
    std::string source = std::string(game_meta_source::kCatalog);
};

/** One row from game_aliases (for Catalog tab / identity debugging). */
struct GameMetaAliasRecord {
    std::string alias_kind;
    std::string alias_value;
    std::string system_key;
    std::string game_id;
};

/** Save-browser key for a meta-backed PS2 title: ps2:id:<catalog_game_id>. */
inline std::string ps2_meta_game_key(std::string_view game_id) {
    return std::string("ps2:id:") + std::string(game_id);
}

inline bool is_ps2_meta_game_key(std::string_view game_key) {
    return game_key.size() > 7 && game_key.substr(0, 7) == "ps2:id:";
}

inline std::string game_id_from_ps2_meta_key(std::string_view game_key) {
    if (!is_ps2_meta_game_key(game_key)) {
        return {};
    }
    return std::string(game_key.substr(7));
}

/**
 * Existing Meta sidecar for a ROM: adjacent `.json`, else Games→Meta mirror path
 * when that file exists. Empty if neither is present.
 */
std::filesystem::path guess_rom_meta_json(const std::filesystem::path& content_path);

/**
 * Host-side SQLite index for game titles and secondary ids (title-id, stem, …).
 * Soft-fail: a missing/unwritable DB must not break play or the Users tab.
 */
class GameMetaStore {
public:
    static std::filesystem::path default_path();

    explicit GameMetaStore(std::filesystem::path db_path = default_path());
    ~GameMetaStore();

    GameMetaStore(const GameMetaStore&) = delete;
    GameMetaStore& operator=(const GameMetaStore&) = delete;
    GameMetaStore(GameMetaStore&&) noexcept;
    GameMetaStore& operator=(GameMetaStore&&) noexcept;

    bool ready() const { return db_ != nullptr; }
    const std::filesystem::path& path() const { return db_path_; }

    bool upsert(const GameMetaRecord& row);
    bool upsert_alias(
        std::string_view kind,
        std::string_view value,
        std::string_view system_key,
        std::string_view game_id);

    std::optional<GameMetaRecord> find_by_id(std::string_view game_id) const;
    std::optional<GameMetaRecord> find_by_content_path(std::string_view content_path) const;
    /**
     * Resolve a catalog id or any alias value.
     * When system_key is set, prefer aliases scoped to that system, then global.
     */
    std::optional<GameMetaRecord> resolve(
        std::string_view key,
        std::string_view system_key = {}) const;

    /**
     * Directory scan ingest (DB is authoritative for identity):
     * - Match existing row by content_path, then scanned id, then stem aliases.
     * - If matched: keep DB identity fields; refresh content_path / core;
     *   fold colliding provisional / stem-claiming primaries into catalog_id aliases.
     * - If new: insert from the scanned GameInfo + path.
     * Mutates [info] in place to the surviving DB identity so the live catalog matches.
     */
    void bind_scanned_game(
        GameInfo& info,
        const std::filesystem::path& content_path);

    /**
     * Write/update the Meta JSON sidecar for this row (name, canonical, region, …)
     * so filesystem metadata matches the DB. Soft-fail.
     */
    bool write_meta_sidecar(const GameMetaRecord& row) const;

    /** Upsert every hosted game + standard aliases (soft-fail). Prefers bind_scanned_game. */
    void sync_from_catalog(const GameCatalog& catalog);

    /**
     * Bind a Switch title-id to the best catalog game matching title_hint
     * (prefers unversioned / base display names). Soft-fail.
     */
    bool learn_switch_title_id(std::string_view title_id, std::string_view title_hint);

    /** Flatten aliases into SaveNameHints for the Users/saves browser. */
    SaveNameHints save_name_hints() const;

    /** All game_meta rows (Catalog tab / debugging). Ordered by system, display. */
    std::vector<GameMetaRecord> list_games() const;

    /** All game_aliases rows, optionally filtered to one game_id. */
    std::vector<GameMetaAliasRecord> list_aliases(std::string_view game_id = {}) const;

    /** Record that username played catalog game_id (upserts last_played_at). */
    bool record_user_game(
        std::string_view username,
        std::string_view game_id,
        std::string_view system_key);

    bool remove_user_game(std::string_view username, std::string_view game_id);
    std::size_t remove_user_games_for_system(
        std::string_view username,
        std::string_view system_key);
    /** Rewrite every user_games.game_id from old → new (Catalog id migrate). */
    std::size_t migrate_user_games_game_id(
        std::string_view old_game_id,
        std::string_view new_game_id);
    /** Delete every user_games row for this catalog id. */
    std::size_t remove_user_games_for_game_id(std::string_view game_id);

    /** Delete game_meta row (does not touch aliases / user_games). */
    bool delete_game(std::string_view game_id);
    /** Delete all aliases pointing at game_id. */
    std::size_t delete_aliases_for_game(std::string_view game_id);
    /** Delete one alias by primary key. */
    bool delete_alias(
        std::string_view alias_kind,
        std::string_view alias_value,
        std::string_view system_key);
    /** Point every alias currently on old_game_id at new_game_id. */
    std::size_t reassign_aliases_game_id(
        std::string_view old_game_id,
        std::string_view new_game_id);

    /**
     * Drop aliases for game_id then write the standard catalog set
     * (catalog_id, canonical, content_stem, display_name, match bases).
     */
    void rebuild_standard_aliases(const GameMetaRecord& row);

    std::vector<UserGameRecord> list_user_games(
        std::string_view username = {},
        std::string_view system_key = {}) const;

    /** Upsert Meta JSON / catalog play-mode fields for a game_id. */
    bool upsert_play_modes(const GamePlayModesRecord& row);
    std::optional<GamePlayModesRecord> find_play_modes(std::string_view game_id) const;
    std::vector<GamePlayModesRecord> list_play_modes() const;
    /** Rewrite play_modes.game_id on Catalog id migrate. */
    bool migrate_play_modes_game_id(
        std::string_view old_game_id,
        std::string_view new_game_id);
    bool delete_play_modes(std::string_view game_id);

private:
    bool open();
    bool ensure_schema();
    bool exec(const char* sql);
    bool exec_quiet(const char* sql);
    std::optional<GameMetaRecord> load_by_id_stmt(std::string_view game_id) const;
    std::optional<std::string> lookup_alias_game_id(
        std::string_view kind,
        std::string_view value,
        std::string_view system_key) const;

    std::filesystem::path db_path_;
    sqlite3* db_ = nullptr;
};

/** Open the default meta DB (may be !ready()). */
std::shared_ptr<GameMetaStore> make_game_meta_store();

/** Soft-fail sync helper used after catalog scans. */
void sync_game_meta_from_catalog(const GameCatalog& catalog);

/** Soft-fail: remember username↔catalog game for Users listing / Active. */
void record_user_game_played(
    std::string_view username,
    std::string_view game_id,
    std::string_view system_key);

} // namespace archstreamer
