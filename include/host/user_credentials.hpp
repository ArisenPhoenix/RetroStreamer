#pragma once

#include "common/protocol.hpp"
#include "common/platform/default_platform.hpp"

#include <filesystem>
#include <string>
#include <string_view>

namespace archstreamer {

/** Default password assigned to pre-existing save profiles (must_change=true). */
inline constexpr std::string_view DefaultUserPassword = "archstreamer";

enum class UserAuthResult {
    Ok,
    MustChange,
    Rejected,
    RejectedNewUser,
};

struct UserCredentials {
    std::string password;
    bool must_change = false;
};

std::filesystem::path credentials_path(const std::filesystem::path& user_directory);

/** Write default credentials if the file is missing (existing save dirs). */
void ensure_default_credentials(const std::filesystem::path& user_directory);

/**
 * If the user directory does not exist: create save profile + credentials from
 * the supplied password (must_change=false) when allow_new_users is true;
 * otherwise RejectedNewUser.
 * If it exists: ensure credentials, then verify the password.
 */
UserAuthResult verify_or_create_on_hello(
    const std::filesystem::path& save_root,
    const std::string& username,
    const std::string& password,
    bool allow_new_users = false);

/**
 * Verify current password and set a new one (clears must_change).
 * Returns an ErrorPacket message suitable to send to the client.
 */
ErrorPacket apply_password_change(
    const std::filesystem::path& save_root,
    const PasswordChange& change);

/**
 * Authenticate a ClientHello on an open control stream.
 * On MustChange: sends PasswordChangeRequired, waits for PasswordChange, applies it.
 * Throws std::runtime_error on failure (caller may forward as ErrorPacket).
 * Updates hello.password when a forced change succeeds.
 */
void authenticate_client_hello(
    TcpStream& stream,
    const std::filesystem::path& save_root,
    ClientHello& hello,
    bool allow_new_users = false);

/** Handle a standalone PasswordChange side-channel (Profile change anytime). */
ErrorPacket acknowledge_password_change(
    const std::filesystem::path& save_root,
    const PasswordChange& change);

} // namespace archstreamer
