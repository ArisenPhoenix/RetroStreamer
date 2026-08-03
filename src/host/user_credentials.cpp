#include "host/user_credentials.hpp"

#include "common/serialization.hpp"
#include "host/save_profile.hpp"

#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace archstreamer {
namespace {

UserCredentials read_credentials(const std::filesystem::path& path) {
    std::ifstream in(path);
    if (!in) {
        throw std::runtime_error("unable to read credentials");
    }
    nlohmann::json json;
    try {
        json = nlohmann::json::parse(in);
    } catch (const nlohmann::json::exception& error) {
        throw std::runtime_error(std::string("invalid credentials.json: ") + error.what());
    }
    UserCredentials creds;
    creds.password = json.value("password", "");
    creds.must_change = json.value("must_change", false);
    return creds;
}

void write_credentials(const std::filesystem::path& path, const UserCredentials& creds) {
    nlohmann::json json{
        {"password", creds.password},
        {"must_change", creds.must_change},
    };
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::trunc);
    if (!out) {
        throw std::runtime_error("unable to write credentials");
    }
    out << json.dump(2) << '\n';
}

} // namespace

std::filesystem::path credentials_path(const std::filesystem::path& user_directory) {
    return user_directory / "credentials.json";
}

void ensure_default_credentials(const std::filesystem::path& user_directory) {
    const auto path = credentials_path(user_directory);
    if (std::filesystem::exists(path)) {
        return;
    }
    write_credentials(
        path,
        UserCredentials{std::string(DefaultUserPassword), true});
}

UserAuthResult verify_or_create_on_hello(
    const std::filesystem::path& save_root,
    const std::string& username,
    const std::string& password,
    bool allow_new_users) {
    if (!valid_username(username)) {
        return UserAuthResult::Rejected;
    }
    if (password.empty()) {
        return UserAuthResult::Rejected;
    }

    const auto root =
        save_root.empty() ? default_save_profile_root() : save_root;
    const auto user_directory = root / username;
    const bool existed = std::filesystem::exists(user_directory);

    if (!existed) {
        if (!allow_new_users) {
            return UserAuthResult::RejectedNewUser;
        }
        prepare_save_profile(root, username);
        write_credentials(
            credentials_path(user_directory),
            UserCredentials{password, false});
        return UserAuthResult::Ok;
    }

    ensure_default_credentials(user_directory);
    const auto creds = read_credentials(credentials_path(user_directory));
    if (creds.password != password) {
        return UserAuthResult::Rejected;
    }
    if (creds.must_change) {
        return UserAuthResult::MustChange;
    }
    return UserAuthResult::Ok;
}

ErrorPacket apply_password_change(
    const std::filesystem::path& save_root,
    const PasswordChange& change) {
    if (!valid_username(change.username)) {
        return ErrorPacket{"invalid username"};
    }
    if (change.new_password.empty()) {
        return ErrorPacket{"new password must not be empty"};
    }
    if (change.new_password == change.current_password) {
        return ErrorPacket{"new password must differ from the current password"};
    }

    const auto root =
        save_root.empty() ? default_save_profile_root() : save_root;
    const auto user_directory = root / change.username;
    if (!std::filesystem::exists(user_directory)) {
        return ErrorPacket{"unknown user"};
    }

    ensure_default_credentials(user_directory);
    const auto path = credentials_path(user_directory);
    auto creds = read_credentials(path);
    if (creds.password != change.current_password) {
        return ErrorPacket{"incorrect password"};
    }
    creds.password = change.new_password;
    creds.must_change = false;
    write_credentials(path, creds);
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
