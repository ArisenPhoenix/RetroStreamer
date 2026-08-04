#include "archstreamer/runtime_cadence/user_auth.hpp"

#include "common/protocol.hpp"
#include "common/sha256.hpp"

#include <nlohmann/json.hpp>

#include <fstream>
#include <iomanip>
#include <random>
#include <sstream>
#include <system_error>

namespace archstreamer::cadence {
namespace {

constexpr std::string_view kDefaultPassword = "archstreamer";
constexpr std::string_view kHashPrefix = "v1:";

std::string random_salt_hex() {
    std::random_device rd;
    std::uniform_int_distribution<int> dist(0, 255);
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (int i = 0; i < 16; ++i) {
        out << std::setw(2) << dist(rd);
    }
    return out.str();
}

bool looks_like_v1_hash(std::string_view stored) {
    return stored.size() > kHashPrefix.size() &&
        stored.substr(0, kHashPrefix.size()) == kHashPrefix;
}

UserRecord make_new_user(
    const std::string& username,
    const std::string& display_name,
    std::string_view password,
    bool must_change) {
    UserRecord user;
    user.username = username;
    user.display_name = display_name.empty() ? username : display_name;
    user.password_hash = make_password_hash(password);
    user.must_change = must_change;
    user.created_at = now_epoch_seconds();
    user.updated_at = user.created_at;
    return user;
}

} // namespace

void apply_user_save_paths(UserRecord& user, const std::filesystem::path& save_root) {
    if (save_root.empty() || user.username.empty()) {
        return;
    }
    user.save_root = save_root.lexically_normal().string();
    user.profile_path = (save_root / user.username).lexically_normal().string();
}

std::string make_password_hash(std::string_view password) {
    const auto salt = random_salt_hex();
    const auto digest = sha256_hex(std::string(salt) + ":" + std::string(password));
    return std::string(kHashPrefix) + salt + ":" + digest;
}

bool password_matches(std::string_view password, std::string_view stored) {
    if (password.empty() || stored.empty()) {
        return false;
    }
    if (!looks_like_v1_hash(stored)) {
        // Legacy plaintext from credentials.json import / older mirrors.
        return password == stored;
    }
    // v1:<salt>:<digest>
    const auto body = stored.substr(kHashPrefix.size());
    const auto colon = body.find(':');
    if (colon == std::string_view::npos || colon == 0) {
        return false;
    }
    const auto salt = body.substr(0, colon);
    const auto expected = body.substr(colon + 1);
    const auto digest = sha256_hex(std::string(salt) + ":" + std::string(password));
    return digest == expected;
}

UserAuthResult verify_or_create_user(
    RuntimeStore& store,
    const std::string& username,
    const std::string& password,
    const std::string& display_name,
    bool allow_new_users,
    const std::filesystem::path& save_root) {
    if (!valid_username(username) || password.empty()) {
        return UserAuthResult::Rejected;
    }
    if (!store.ensure_ready()) {
        return UserAuthResult::Unavailable;
    }

    auto existing = store.find_user(username);
    if (!existing.has_value()) {
        if (!allow_new_users) {
            return UserAuthResult::RejectedNewUser;
        }
        auto user = make_new_user(username, display_name, password, false);
        apply_user_save_paths(user, save_root);
        if (!store.upsert_user(user)) {
            return UserAuthResult::Unavailable;
        }
        return UserAuthResult::Ok;
    }

    if (!password_matches(password, existing->password_hash)) {
        return UserAuthResult::Rejected;
    }

    // Upgrade plaintext / incomplete rows to salted hashes on successful login.
    bool dirty = false;
    if (!looks_like_v1_hash(existing->password_hash) || existing->display_name.empty()) {
        existing->password_hash = make_password_hash(password);
        if (existing->display_name.empty()) {
            existing->display_name = display_name.empty() ? username : display_name;
        }
        if (existing->created_at <= 0) {
            existing->created_at = now_epoch_seconds();
        }
        dirty = true;
    }
    if (existing->profile_path.empty() && !save_root.empty()) {
        apply_user_save_paths(*existing, save_root);
        dirty = true;
    }
    if (dirty) {
        existing->updated_at = now_epoch_seconds();
        (void)store.upsert_user(*existing);
    }

    if (existing->must_change) {
        return UserAuthResult::MustChange;
    }
    return UserAuthResult::Ok;
}

std::string change_user_password(
    RuntimeStore& store,
    const std::string& username,
    const std::string& current_password,
    const std::string& new_password) {
    if (!valid_username(username)) {
        return "invalid username";
    }
    if (new_password.empty()) {
        return "new password must not be empty";
    }
    if (new_password == current_password) {
        return "new password must differ from the current password";
    }
    if (!store.ensure_ready()) {
        return "user store unavailable";
    }
    auto existing = store.find_user(username);
    if (!existing.has_value()) {
        return "unknown user";
    }
    if (!password_matches(current_password, existing->password_hash)) {
        return "incorrect password";
    }
    existing->password_hash = make_password_hash(new_password);
    existing->must_change = false;
    existing->updated_at = now_epoch_seconds();
    if (!store.upsert_user(*existing)) {
        return "failed to update password";
    }
    return {};
}

bool ensure_default_user(
    RuntimeStore& store,
    const std::string& username,
    const std::string& display_name,
    const std::filesystem::path& save_root) {
    if (!valid_username(username) || !store.ensure_ready()) {
        return false;
    }
    if (auto existing = store.find_user(username); existing.has_value()) {
        if (existing->profile_path.empty() && !save_root.empty()) {
            apply_user_save_paths(*existing, save_root);
            existing->updated_at = now_epoch_seconds();
            (void)store.upsert_user(*existing);
        }
        return true;
    }
    auto user = make_new_user(username, display_name, kDefaultPassword, true);
    apply_user_save_paths(user, save_root);
    return store.upsert_user(user);
}

std::size_t backfill_user_profile_paths(
    RuntimeStore& store,
    const std::filesystem::path& save_root) {
    if (save_root.empty() || !store.ensure_ready()) {
        return 0;
    }
    std::size_t updated = 0;
    for (auto user : store.list_users()) {
        if (!user.profile_path.empty()) {
            continue;
        }
        const auto profile = save_root / user.username;
        std::error_code ec;
        if (!std::filesystem::is_directory(profile, ec) || ec) {
            continue;
        }
        apply_user_save_paths(user, save_root);
        user.updated_at = now_epoch_seconds();
        if (store.upsert_user(user)) {
            ++updated;
        }
    }
    return updated;
}

std::size_t import_users_from_save_root(
    RuntimeStore& store,
    const std::filesystem::path& save_root) {
    if (save_root.empty() || !store.ensure_ready()) {
        return 0;
    }
    std::error_code ec;
    if (!std::filesystem::is_directory(save_root, ec) || ec) {
        return 0;
    }

    std::size_t imported = 0;
    for (const auto& entry : std::filesystem::directory_iterator(save_root, ec)) {
        if (ec || !entry.is_directory()) {
            continue;
        }
        const auto username = entry.path().filename().string();
        if (!valid_username(username)) {
            continue;
        }
        if (auto existing = store.find_user(username); existing.has_value()) {
            if (existing->profile_path.empty()) {
                apply_user_save_paths(*existing, save_root);
                existing->updated_at = now_epoch_seconds();
                if (store.upsert_user(*existing)) {
                    ++imported;
                }
            }
            continue;
        }
        const auto cred_path = entry.path() / "credentials.json";
        if (!std::filesystem::exists(cred_path)) {
            if (ensure_default_user(store, username, username, save_root)) {
                ++imported;
            }
            continue;
        }
        try {
            std::ifstream in(cred_path);
            nlohmann::json json;
            in >> json;
            const auto password = json.value("password", std::string(kDefaultPassword));
            const bool must_change = json.value("must_change", true);
            if (password.empty()) {
                continue;
            }
            UserRecord user = make_new_user(username, username, password, must_change);
            apply_user_save_paths(user, save_root);
            if (store.upsert_user(user)) {
                ++imported;
            }
        } catch (...) {
            continue;
        }
    }
    return imported;
}

bool write_credentials_mirror(
    const std::filesystem::path& user_directory,
    std::string_view plaintext_password,
    bool must_change) {
    if (user_directory.empty() || plaintext_password.empty()) {
        return false;
    }
    nlohmann::json json{
        {"password", std::string(plaintext_password)},
        {"must_change", must_change},
    };
    std::error_code ec;
    std::filesystem::create_directories(user_directory, ec);
    std::ofstream out(user_directory / "credentials.json", std::ios::trunc);
    if (!out) {
        return false;
    }
    out << json.dump(2) << '\n';
    return static_cast<bool>(out);
}

} // namespace archstreamer::cadence
