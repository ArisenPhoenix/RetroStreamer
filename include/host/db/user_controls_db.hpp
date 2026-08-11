#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace archstreamer {

/** Cap for pull/push controls DB blobs (real packs are a few KB). */
inline constexpr std::size_t kControlsDbMaxBytes = 2u * 1024u * 1024u;

inline constexpr std::string_view kControlsKindButtonMap = "button_map";
inline constexpr std::string_view kControlsKindOverlayProfiles = "overlay_profiles";
inline constexpr std::string_view kControlsDbFileName = "controls.sqlite";

struct UserControlsRow {
    std::string username;
    std::string kind;
    std::string document_json;
    int version = 1;
    std::int64_t updated_at = 0;
};

/** <user_directory>/controls.sqlite */
std::filesystem::path user_controls_db_path(const std::filesystem::path& user_directory);

std::filesystem::path user_controls_db_path_for(
    const std::filesystem::path& save_root,
    std::string_view username);

/** Read the on-disk DB file as bytes (empty if missing). */
std::vector<std::uint8_t> read_user_controls_db_file(const std::filesystem::path& db_path);

/**
 * Replace the profile controls.sqlite with @bytes (must be a valid pack ≤ max size).
 * Creates parent directories. Returns false on validation/IO failure.
 */
bool write_user_controls_db_file(
    const std::filesystem::path& db_path,
    const std::vector<std::uint8_t>& bytes,
    std::string* error_out = nullptr);

/** Upsert one row into the profile DB (creates DB/table if needed). */
bool upsert_user_controls_row(
    const std::filesystem::path& db_path,
    const UserControlsRow& row);

std::optional<UserControlsRow> find_user_controls_row(
    const std::filesystem::path& db_path,
    std::string_view username,
    std::string_view kind);

/**
 * Validate that @bytes is a readable SQLite pack with user_controls, and that
 * every row's username matches @expected_username (or pack is empty).
 */
bool validate_controls_db_pack(
    const std::vector<std::uint8_t>& bytes,
    std::string_view expected_username,
    std::string* error_out = nullptr);

} // namespace archstreamer
