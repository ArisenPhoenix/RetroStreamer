#include "archstreamer/runtime_cadence/db_store.hpp"
#include "archstreamer/runtime_cadence/file_store.hpp"
#include "archstreamer/runtime_cadence/protocol.hpp"
#include "common/platform/paths.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <system_error>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <spawn.h>
#include <sys/wait.h>

extern char** environ;
#endif

namespace archstreamer::cadence {

std::filesystem::path default_cadence_socket_path() {
#if !defined(_WIN32)
    if (const char* runtime = std::getenv("XDG_RUNTIME_DIR");
        runtime != nullptr && runtime[0] != '\0') {
        return std::filesystem::path(runtime) / "archstreamer" / "cadence.sock";
    }
#endif
    return default_cadence_data_root() / "cadence.sock";
}

std::filesystem::path default_cadence_sidecar_path() {
#if !defined(_WIN32)
    // Prefer sibling of this process (host_runner / archstreamer_gui).
    std::error_code ec;
    const auto self = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (!ec) {
        return self.parent_path() / "archstreamer_cadence";
    }
#endif
    return "archstreamer_cadence";
}

DbRuntimeStore::DbRuntimeStore(
    std::filesystem::path socket_path,
    std::filesystem::path sidecar_path)
    : socket_path_(socket_path.empty() ? default_cadence_socket_path() : std::move(socket_path))
    , sidecar_path_(sidecar_path.empty() ? default_cadence_sidecar_path() : std::move(sidecar_path)) {}

DbRuntimeStore::~DbRuntimeStore() {
    disconnect();
}

void DbRuntimeStore::disconnect() {
#if !defined(_WIN32)
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
#endif
}

bool DbRuntimeStore::connect() {
#if defined(_WIN32)
    return false;
#else
    if (fd_ >= 0) {
        return true;
    }
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return false;
    }
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    const auto path = socket_path_.string();
    if (path.size() >= sizeof(addr.sun_path)) {
        ::close(fd);
        return false;
    }
    std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path.c_str());
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return false;
    }
    fd_ = fd;
    return true;
#endif
}

bool DbRuntimeStore::spawn_sidecar() {
#if defined(_WIN32)
    return false;
#else
    std::error_code ec;
    std::filesystem::create_directories(socket_path_.parent_path(), ec);

    const auto bin = sidecar_path_.string();
    const auto sock = socket_path_.string();
    const auto db = (default_cadence_data_root() / "cadence.sqlite").string();

    char* argv[] = {
        const_cast<char*>(bin.c_str()),
        const_cast<char*>("--socket"),
        const_cast<char*>(sock.c_str()),
        const_cast<char*>("--db"),
        const_cast<char*>(db.c_str()),
        nullptr,
    };
    pid_t pid = 0;
    const int rc = posix_spawn(&pid, bin.c_str(), nullptr, nullptr, argv, environ);
    if (rc != 0) {
        std::cerr << "cadence(db): failed to spawn " << bin << " (errno " << rc << ")\n";
        return false;
    }
    // Detach: do not wait; sidecar daemonizes its accept loop.
    return true;
#endif
}

bool DbRuntimeStore::ensure_ready() {
    std::lock_guard lock(mutex_);
#if defined(_WIN32)
    std::cerr << "cadence(db): Windows sidecar not implemented; use file cadence\n";
    return false;
#else
    if (connect()) {
        return true;
    }
    if (!spawn_sidecar()) {
        return false;
    }
    for (int i = 0; i < 40; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (connect()) {
            // Prove the protocol with a ping.
            std::string resp;
            if (request("ping", "{}", resp)) {
                return true;
            }
            disconnect();
        }
    }
    std::cerr << "cadence(db): sidecar did not become ready at " << socket_path_ << '\n';
    return false;
#endif
}

bool DbRuntimeStore::request(
    const std::string& op,
    const std::string& body_json,
    std::string& response_json) {
#if defined(_WIN32)
    (void)op;
    (void)body_json;
    (void)response_json;
    return false;
#else
    if (fd_ < 0 && !connect()) {
        return false;
    }
    nlohmann::json req;
    try {
        req = body_json.empty() || body_json == "{}"
            ? nlohmann::json::object()
            : nlohmann::json::parse(body_json);
    } catch (const nlohmann::json::exception&) {
        req = nlohmann::json::object();
    }
    req["op"] = op;
    if (!write_frame(fd_, req.dump())) {
        disconnect();
        return false;
    }
    if (!read_frame(fd_, response_json)) {
        disconnect();
        return false;
    }
    return true;
#endif
}

bool DbRuntimeStore::upsert_user(const UserRecord& user) {
    std::lock_guard lock(mutex_);
    auto body = user_to_json(user);
    std::string resp;
    if (!request("upsert_user", body.dump(), resp)) {
        return false;
    }
    try {
        return nlohmann::json::parse(resp).value("ok", false);
    } catch (const nlohmann::json::exception&) {
        return false;
    }
}

std::optional<UserRecord> DbRuntimeStore::find_user(const std::string& username) {
    std::lock_guard lock(mutex_);
    nlohmann::json body{{"username", username}};
    std::string resp;
    if (!request("find_user", body.dump(), resp)) {
        return std::nullopt;
    }
    try {
        const auto j = nlohmann::json::parse(resp);
        if (!j.value("ok", false) || !j.contains("user") || j["user"].is_null()) {
            return std::nullopt;
        }
        return user_from_json(j["user"]);
    } catch (const nlohmann::json::exception&) {
        return std::nullopt;
    }
}

bool DbRuntimeStore::delete_user(const std::string& username) {
    std::lock_guard lock(mutex_);
    nlohmann::json body{{"username", username}};
    std::string resp;
    if (!request("delete_user", body.dump(), resp)) {
        return false;
    }
    try {
        return nlohmann::json::parse(resp).value("ok", false);
    } catch (const nlohmann::json::exception&) {
        return false;
    }
}

std::vector<UserRecord> DbRuntimeStore::list_users() {
    std::lock_guard lock(mutex_);
    std::string resp;
    if (!request("list_users", "{}", resp)) {
        return {};
    }
    try {
        const auto j = nlohmann::json::parse(resp);
        if (!j.value("ok", false) || !j.contains("users")) {
            return {};
        }
        std::vector<UserRecord> out;
        for (const auto& item : j["users"]) {
            out.push_back(user_from_json(item));
        }
        return out;
    } catch (const nlohmann::json::exception&) {
        return {};
    }
}

bool DbRuntimeStore::upsert_controls(const ControlsRecord& controls) {
    std::lock_guard lock(mutex_);
    std::string resp;
    if (!request("upsert_controls", controls_to_json(controls).dump(), resp)) {
        return false;
    }
    try {
        return nlohmann::json::parse(resp).value("ok", false);
    } catch (const nlohmann::json::exception&) {
        return false;
    }
}

std::optional<ControlsRecord> DbRuntimeStore::find_controls(
    const std::string& username,
    const std::string& kind) {
    std::lock_guard lock(mutex_);
    nlohmann::json body{
        {"username", username},
        {"kind", kind.empty() ? std::string(kControlsKindButtonMap) : kind},
    };
    std::string resp;
    if (!request("find_controls", body.dump(), resp)) {
        return std::nullopt;
    }
    try {
        const auto j = nlohmann::json::parse(resp);
        if (!j.value("ok", false) || !j.contains("controls") || j["controls"].is_null()) {
            return std::nullopt;
        }
        return controls_from_json(j["controls"]);
    } catch (const nlohmann::json::exception&) {
        return std::nullopt;
    }
}

std::vector<ControlsRecord> DbRuntimeStore::list_controls() {
    std::lock_guard lock(mutex_);
    std::string resp;
    if (!request("list_controls", "{}", resp)) {
        return {};
    }
    try {
        const auto j = nlohmann::json::parse(resp);
        if (!j.value("ok", false) || !j.contains("controls")) {
            return {};
        }
        std::vector<ControlsRecord> out;
        for (const auto& item : j["controls"]) {
            out.push_back(controls_from_json(item));
        }
        return out;
    } catch (const nlohmann::json::exception&) {
        return {};
    }
}

bool DbRuntimeStore::upsert_session(const SessionRecord& session) {
    std::lock_guard lock(mutex_);
    std::string resp;
    if (!request("upsert_session", session_to_json(session).dump(), resp)) {
        return false;
    }
    try {
        return nlohmann::json::parse(resp).value("ok", false);
    } catch (const nlohmann::json::exception&) {
        return false;
    }
}

bool DbRuntimeStore::end_session(const std::string& session_id, const std::string& end_reason) {
    std::lock_guard lock(mutex_);
    nlohmann::json body{{"session_id", session_id}, {"end_reason", end_reason}};
    std::string resp;
    if (!request("end_session", body.dump(), resp)) {
        return false;
    }
    try {
        return nlohmann::json::parse(resp).value("ok", false);
    } catch (const nlohmann::json::exception&) {
        return false;
    }
}

std::optional<SessionRecord> DbRuntimeStore::find_session(const std::string& session_id) {
    std::lock_guard lock(mutex_);
    nlohmann::json body{{"session_id", session_id}};
    std::string resp;
    if (!request("find_session", body.dump(), resp)) {
        return std::nullopt;
    }
    try {
        const auto j = nlohmann::json::parse(resp);
        if (!j.value("ok", false) || !j.contains("session") || j["session"].is_null()) {
            return std::nullopt;
        }
        return session_from_json(j["session"]);
    } catch (const nlohmann::json::exception&) {
        return std::nullopt;
    }
}

std::vector<SessionRecord> DbRuntimeStore::list_sessions(bool active_only) {
    std::lock_guard lock(mutex_);
    nlohmann::json body{{"active_only", active_only}};
    std::string resp;
    if (!request("list_sessions", body.dump(), resp)) {
        return {};
    }
    try {
        const auto j = nlohmann::json::parse(resp);
        if (!j.value("ok", false) || !j.contains("sessions")) {
            return {};
        }
        std::vector<SessionRecord> out;
        for (const auto& item : j["sessions"]) {
            out.push_back(session_from_json(item));
        }
        return out;
    } catch (const nlohmann::json::exception&) {
        return {};
    }
}

bool DbRuntimeStore::claim_resource(const ResourceClaim& claim) {
    std::lock_guard lock(mutex_);
    std::string resp;
    if (!request("claim_resource", claim_to_json(claim).dump(), resp)) {
        return false;
    }
    try {
        return nlohmann::json::parse(resp).value("ok", false);
    } catch (const nlohmann::json::exception&) {
        return false;
    }
}

bool DbRuntimeStore::release_resource(
    const std::string& session_id,
    const std::string& resource_type,
    const std::string& resource_name) {
    std::lock_guard lock(mutex_);
    nlohmann::json body{
        {"session_id", session_id},
        {"resource_type", resource_type},
        {"resource_name", resource_name},
    };
    std::string resp;
    if (!request("release_resource", body.dump(), resp)) {
        return false;
    }
    try {
        return nlohmann::json::parse(resp).value("ok", false);
    } catch (const nlohmann::json::exception&) {
        return false;
    }
}

bool DbRuntimeStore::release_session_resources(const std::string& session_id) {
    std::lock_guard lock(mutex_);
    nlohmann::json body{{"session_id", session_id}};
    std::string resp;
    if (!request("release_session_resources", body.dump(), resp)) {
        return false;
    }
    try {
        return nlohmann::json::parse(resp).value("ok", false);
    } catch (const nlohmann::json::exception&) {
        return false;
    }
}

std::vector<ResourceClaim> DbRuntimeStore::list_claims(bool held_only) {
    std::lock_guard lock(mutex_);
    nlohmann::json body{{"held_only", held_only}};
    std::string resp;
    if (!request("list_claims", body.dump(), resp)) {
        return {};
    }
    try {
        const auto j = nlohmann::json::parse(resp);
        if (!j.value("ok", false) || !j.contains("claims")) {
            return {};
        }
        std::vector<ResourceClaim> out;
        for (const auto& item : j["claims"]) {
            out.push_back(claim_from_json(item));
        }
        return out;
    } catch (const nlohmann::json::exception&) {
        return {};
    }
}

std::optional<ResourceClaim> DbRuntimeStore::find_held_resource(
    const std::string& resource_type,
    const std::string& resource_name) {
    std::lock_guard lock(mutex_);
    nlohmann::json body{
        {"resource_type", resource_type},
        {"resource_name", resource_name},
    };
    std::string resp;
    if (!request("find_held_resource", body.dump(), resp)) {
        return std::nullopt;
    }
    try {
        const auto j = nlohmann::json::parse(resp);
        if (!j.value("ok", false) || !j.contains("claim") || j["claim"].is_null()) {
            return std::nullopt;
        }
        return claim_from_json(j["claim"]);
    } catch (const nlohmann::json::exception&) {
        return std::nullopt;
    }
}

bool DbRuntimeStore::record_event(const RuntimeEvent& event) {
    std::lock_guard lock(mutex_);
    auto body = event_to_json(event);
    std::string resp;
    if (!request("record_event", body.dump(), resp)) {
        return false;
    }
    try {
        const bool ok = nlohmann::json::parse(resp).value("ok", false);
        if (ok) {
            std::cerr << "cadence(db): " << event.kind;
            if (!event.username.empty()) {
                std::cerr << " user=" << event.username;
            }
            if (!event.detail.empty()) {
                std::cerr << " " << event.detail;
            }
            std::cerr << '\n';
        }
        return ok;
    } catch (const nlohmann::json::exception&) {
        return false;
    }
}

std::vector<RuntimeEvent> DbRuntimeStore::recent_events(
    const std::string& day,
    std::size_t limit) {
    std::lock_guard lock(mutex_);
    nlohmann::json body{{"day", day}, {"limit", limit}};
    std::string resp;
    if (!request("recent_events", body.dump(), resp)) {
        return {};
    }
    try {
        const auto j = nlohmann::json::parse(resp);
        if (!j.value("ok", false) || !j.contains("events")) {
            return {};
        }
        std::vector<RuntimeEvent> out;
        for (const auto& item : j["events"]) {
            out.push_back(event_from_json(item));
        }
        return out;
    } catch (const nlohmann::json::exception&) {
        return {};
    }
}

} // namespace archstreamer::cadence
