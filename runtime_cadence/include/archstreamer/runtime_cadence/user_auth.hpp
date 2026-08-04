#pragma once

#include "archstreamer/runtime_cadence/store.hpp"
#include "archstreamer/runtime_cadence/types.hpp"

#include <filesystem>
#include <string>
#include <string_view>

namespace archstreamer::cadence {

/** Auth outcomes shared by host / GUI / future mobile-adjacent tools. */
enum class UserAuthResult {
    Ok,
    MustChange,
    Rejected,
    RejectedNewUser,
    /** Store not ready (sidecar down, I/O failure). */
    Unavailable,
};

/** Password hash format: `v1:<salt_hex>:<sha256_hex(salt:password)>`. */
std::string make_password_hash(std::string_view password);

/**
 * True if `password` matches `stored`.
 * Also accepts legacy plaintext (no `v1:` prefix) for one-shot migration.
 */
bool password_matches(std::string_view password, std::string_view stored);

/**
 * Create or verify a user in the cadence store.
 * Does not touch save-profile directories — callers still prepare those separately.
 */
UserAuthResult verify_or_create_user(
    RuntimeStore& store,
    const std::string& username,
    const std::string& password,
    const std::string& display_name,
    bool allow_new_users,
    const std::filesystem::path& save_root = {});

/**
 * Verify current password and set a new hash (clears must_change).
 * Returns empty string on success, otherwise a short error message.
 */
std::string change_user_password(
    RuntimeStore& store,
    const std::string& username,
    const std::string& current_password,
    const std::string& new_password);

/**
 * Ensure a cadence user exists with the default password and must_change=true
 * (host Saves tab / legacy save dirs). No-op if the user already exists.
 * When save_root is non-empty, sets profile_path / save_root on create or backfill.
 */
bool ensure_default_user(
    RuntimeStore& store,
    const std::string& username,
    const std::string& display_name = {},
    const std::filesystem::path& save_root = {});

/**
 * Import plaintext credentials.json under each `<save_root>/<user>/`.
 * Skips usernames already present in the store (but backfills empty paths).
 * Returns count imported or path-backfilled.
 */
std::size_t import_users_from_save_root(
    RuntimeStore& store,
    const std::filesystem::path& save_root);

/**
 * For each cadence user missing profile_path, set from save_root/username when
 * that directory exists. Returns count updated.
 */
std::size_t backfill_user_profile_paths(
    RuntimeStore& store,
    const std::filesystem::path& save_root);

/** Apply save_root + profile_path onto a UserRecord. */
void apply_user_save_paths(
    UserRecord& user,
    const std::filesystem::path& save_root);

/** Dual-write mirror so older hosts can still read credentials.json. */
bool write_credentials_mirror(
    const std::filesystem::path& user_directory,
    std::string_view plaintext_password,
    bool must_change);

} // namespace archstreamer::cadence
