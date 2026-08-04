#include "host/user_credentials.hpp"

#include "archstreamer/runtime_cadence/cadence.hpp"
#include "common/serialization.hpp"
#include "host/save_profile.hpp"

#include <stdexcept>

namespace archstreamer {
namespace {

std::shared_ptr<cadence::RuntimeStore> cadence_store() {
    return cadence::make_runtime_store();
}

UserAuthResult to_host_result(cadence::UserAuthResult result) {
    switch (result) {
    case cadence::UserAuthResult::Ok:
        return UserAuthResult::Ok;
    case cadence::UserAuthResult::MustChange:
        return UserAuthResult::MustChange;
    case cadence::UserAuthResult::RejectedNewUser:
        return UserAuthResult::RejectedNewUser;
    case cadence::UserAuthResult::Unavailable:
    case cadence::UserAuthResult::Rejected:
    default:
        return UserAuthResult::Rejected;
    }
}

} // namespace

std::filesystem::path credentials_path(const std::filesystem::path& user_directory) {
    return user_directory / "credentials.json";
}

void ensure_default_credentials(const std::filesystem::path& user_directory) {
    const auto username = user_directory.filename().string();
    auto store = cadence_store();
    (void)cadence::ensure_default_user(*store, username);
    // Mirror only if missing so we never clobber a real password with the default.
    if (!std::filesystem::exists(credentials_path(user_directory))) {
        (void)cadence::write_credentials_mirror(
            user_directory,
            DefaultUserPassword,
            true);
    }
}

UserAuthResult verify_or_create_on_hello(
    const std::filesystem::path& save_root,
    const std::string& username,
    const std::string& password,
    bool allow_new_users) {
    if (!valid_username(username) || password.empty()) {
        return UserAuthResult::Rejected;
    }

    const auto root =
        save_root.empty() ? default_save_profile_root() : save_root;
    auto store = cadence_store();
    if (!store->ensure_ready()) {
        return UserAuthResult::Rejected;
    }
    (void)cadence::import_users_from_save_root(*store, root);

    const auto user_directory = root / username;
    const bool in_store = store->find_user(username).has_value();
    const bool on_disk = std::filesystem::exists(user_directory);

    if (!in_store && !on_disk) {
        if (!allow_new_users) {
            return UserAuthResult::RejectedNewUser;
        }
        prepare_save_profile(root, username);
    } else if (!on_disk) {
        // Cadence knows the user; ensure the save profile tree exists for blobs.
        prepare_save_profile(root, username);
    } else if (!in_store) {
        // Disk profile survived but cadence missed the import — seed defaults then re-import.
        ensure_default_credentials(user_directory);
        (void)cadence::import_users_from_save_root(*store, root);
    }

    const auto cadence_result = cadence::verify_or_create_user(
        *store,
        username,
        password,
        username,
        allow_new_users);
    if (cadence_result == cadence::UserAuthResult::Ok ||
        cadence_result == cadence::UserAuthResult::MustChange) {
        const bool must_change = cadence_result == cadence::UserAuthResult::MustChange;
        (void)cadence::write_credentials_mirror(user_directory, password, must_change);
    }
    return to_host_result(cadence_result);
}

ErrorPacket apply_password_change(
    const std::filesystem::path& save_root,
    const PasswordChange& change) {
    const auto root =
        save_root.empty() ? default_save_profile_root() : save_root;
    auto store = cadence_store();
    (void)cadence::import_users_from_save_root(*store, root);

    const auto error = cadence::change_user_password(
        *store,
        change.username,
        change.current_password,
        change.new_password);
    if (!error.empty()) {
        return ErrorPacket{error};
    }

    const auto user_directory = root / change.username;
    if (!std::filesystem::exists(user_directory)) {
        prepare_save_profile(root, change.username);
    }
    (void)cadence::write_credentials_mirror(
        user_directory,
        change.new_password,
        false);
    return ErrorPacket{"password updated"};
}

void authenticate_client_hello(
    TcpStream& stream,
    const std::filesystem::path& save_root,
    ClientHello& hello,
    bool allow_new_users) {
    const auto result = verify_or_create_on_hello(
        save_root, hello.username, hello.password, allow_new_users);
    if (result == UserAuthResult::RejectedNewUser) {
        throw std::runtime_error("new users are not allowed on this host");
    }
    if (result == UserAuthResult::Rejected) {
        throw std::runtime_error("incorrect password or invalid credentials");
    }
    if (result == UserAuthResult::Ok) {
        return;
    }

    stream.send_packet(serialize_packet(PasswordChangeRequired{}));

    const auto packet = stream.receive_packet();
    if (!packet.has_value()) {
        throw std::runtime_error("disconnected while waiting for password change");
    }
    const auto payload = deserialize_packet(*packet);
    const auto* change = std::get_if<PasswordChange>(&payload);
    if (change == nullptr) {
        throw std::runtime_error("expected PasswordChange after PasswordChangeRequired");
    }
    if (change->username != hello.username) {
        throw std::runtime_error("password change username mismatch");
    }
    if (change->current_password != hello.password) {
        throw std::runtime_error("incorrect password");
    }

    const auto ack = apply_password_change(save_root, *change);
    if (ack.message != "password updated") {
        throw std::runtime_error(ack.message);
    }
    hello.password = change->new_password;
}

ErrorPacket acknowledge_password_change(
    const std::filesystem::path& save_root,
    const PasswordChange& change) {
    return apply_password_change(save_root, change);
}

} // namespace archstreamer
