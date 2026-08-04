#include "archstreamer/runtime_cadence/file_store.hpp"

#include "archstreamer/runtime_cadence/protocol.hpp"
#include "common/platform/paths.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <system_error>

namespace archstreamer::cadence {

std::filesystem::path default_cadence_data_root() {
#if defined(_WIN32)
    if (const char* local = std::getenv("LOCALAPPDATA");
        local != nullptr && local[0] != '\0') {
        return std::filesystem::path(local) / "archstreamer" / "cadence";
    }
#endif
    const auto home = user_home_directory();
    if (!home.empty()) {
        return std::filesystem::path(home) / ".local/share/archstreamer/cadence";
    }
    return std::filesystem::current_path() / "archstreamer-cadence";
}

FileRuntimeStore::FileRuntimeStore(std::filesystem::path root)
    : root_(root.empty() ? default_cadence_data_root() : std::move(root)) {}

std::filesystem::path FileRuntimeStore::users_path() const {
    return root_ / "users.json";
}

std::filesystem::path FileRuntimeStore::sessions_path() const {
    return root_ / "sessions.json";
}

std::filesystem::path FileRuntimeStore::claims_path() const {
    return root_ / "claims.json";
}

std::filesystem::path FileRuntimeStore::events_path(const std::string& day) const {
    return root_ / ("events_" + day + ".jsonl");
}

std::string FileRuntimeStore::claim_key(const std::string& type, const std::string& name) {
    return type + "\n" + name;
}

nlohmann::json FileRuntimeStore::load_object_file(const std::filesystem::path& path) const {
    nlohmann::json root = nlohmann::json::object();
    std::ifstream in(path);
    if (!in) {
        return root;
    }
    try {
        in >> root;
        if (!root.is_object()) {
            return nlohmann::json::object();
        }
        return root;
    } catch (const nlohmann::json::exception&) {
        return nlohmann::json::object();
    }
}

bool FileRuntimeStore::save_object_file(
    const std::filesystem::path& path,
    const nlohmann::json& root) const {
    std::ofstream out(path, std::ios::trunc);
    if (!out) {
        return false;
    }
    out << root.dump(2) << '\n';
    return static_cast<bool>(out);
}

bool FileRuntimeStore::ensure_ready_unlocked() {
    std::error_code ec;
    std::filesystem::create_directories(root_, ec);
    if (ec) {
        std::cerr << "cadence(file): failed to create " << root_ << ": " << ec.message() << '\n';
        return false;
    }
    return true;
}

bool FileRuntimeStore::ensure_ready() {
    std::lock_guard lock(mutex_);
    return ensure_ready_unlocked();
}

bool FileRuntimeStore::upsert_user(const UserRecord& user) {
    if (user.username.empty()) {
        return false;
    }
    std::lock_guard lock(mutex_);
    if (!ensure_ready_unlocked()) {
        return false;
    }
    nlohmann::json root = nlohmann::json::object();
    {
        std::ifstream in(users_path());
        if (in) {
            try {
                in >> root;
                if (!root.is_object()) {
                    root = nlohmann::json::object();
                }
            } catch (const nlohmann::json::exception&) {
                root = nlohmann::json::object();
            }
        }
    }
    UserRecord stored = user;
    if (stored.updated_at <= 0) {
        stored.updated_at = now_epoch_seconds();
    }
    root[stored.username] = user_to_json(stored);
    std::ofstream out(users_path(), std::ios::trunc);
    if (!out) {
        return false;
    }
    out << root.dump(2) << '\n';
    return static_cast<bool>(out);
}

std::optional<UserRecord> FileRuntimeStore::find_user(const std::string& username) {
    if (username.empty()) {
        return std::nullopt;
    }
    std::lock_guard lock(mutex_);
    std::ifstream in(users_path());
    if (!in) {
        return std::nullopt;
    }
    try {
        nlohmann::json root;
        in >> root;
        if (!root.is_object() || !root.contains(username)) {
            return std::nullopt;
        }
        return user_from_json(root.at(username));
    } catch (const nlohmann::json::exception&) {
        return std::nullopt;
    }
}

bool FileRuntimeStore::delete_user(const std::string& username) {
    if (username.empty()) {
        return false;
    }
    std::lock_guard lock(mutex_);
    if (!ensure_ready_unlocked()) {
        return false;
    }
    nlohmann::json root = nlohmann::json::object();
    {
        std::ifstream in(users_path());
        if (in) {
            try {
                in >> root;
                if (!root.is_object()) {
                    root = nlohmann::json::object();
                }
            } catch (const nlohmann::json::exception&) {
                root = nlohmann::json::object();
            }
        }
    }
    if (!root.contains(username)) {
        return true;
    }
    root.erase(username);
    std::ofstream out(users_path(), std::ios::trunc);
    if (!out) {
        return false;
    }
    out << root.dump(2) << '\n';
    return static_cast<bool>(out);
}

std::vector<UserRecord> FileRuntimeStore::list_users() {
    std::lock_guard lock(mutex_);
    std::ifstream in(users_path());
    if (!in) {
        return {};
    }
    try {
        nlohmann::json root;
        in >> root;
        if (!root.is_object()) {
            return {};
        }
        std::vector<UserRecord> out;
        out.reserve(root.size());
        for (auto it = root.begin(); it != root.end(); ++it) {
            auto user = user_from_json(it.value());
            if (user.username.empty()) {
                user.username = it.key();
            }
            out.push_back(std::move(user));
        }
        std::sort(out.begin(), out.end(), [](const UserRecord& a, const UserRecord& b) {
            return a.username < b.username;
        });
        return out;
    } catch (const nlohmann::json::exception&) {
        return {};
    }
}

bool FileRuntimeStore::upsert_session(const SessionRecord& session) {
    if (session.session_id.empty()) {
        return false;
    }
    std::lock_guard lock(mutex_);
    if (!ensure_ready_unlocked()) {
        return false;
    }
    auto root = load_object_file(sessions_path());
    SessionRecord stored = session;
    if (stored.started_at <= 0) {
        stored.started_at = now_epoch_seconds();
    }
    root[stored.session_id] = session_to_json(stored);
    return save_object_file(sessions_path(), root);
}

bool FileRuntimeStore::end_session(const std::string& session_id, const std::string& end_reason) {
    if (session_id.empty()) {
        return false;
    }
    std::lock_guard lock(mutex_);
    if (!ensure_ready_unlocked()) {
        return false;
    }
    auto root = load_object_file(sessions_path());
    if (!root.contains(session_id)) {
        return false;
    }
    auto session = session_from_json(root.at(session_id));
    session.ended_at = now_epoch_seconds();
    session.end_reason = end_reason;
    root[session_id] = session_to_json(session);
    return save_object_file(sessions_path(), root);
}

std::optional<SessionRecord> FileRuntimeStore::find_session(const std::string& session_id) {
    if (session_id.empty()) {
        return std::nullopt;
    }
    std::lock_guard lock(mutex_);
    const auto root = load_object_file(sessions_path());
    if (!root.contains(session_id)) {
        return std::nullopt;
    }
    return session_from_json(root.at(session_id));
}

std::vector<SessionRecord> FileRuntimeStore::list_sessions(bool active_only) {
    std::lock_guard lock(mutex_);
    const auto root = load_object_file(sessions_path());
    std::vector<SessionRecord> out;
    for (auto it = root.begin(); it != root.end(); ++it) {
        auto session = session_from_json(it.value());
        if (session.session_id.empty()) {
            session.session_id = it.key();
        }
        if (active_only && session.ended_at != 0) {
            continue;
        }
        out.push_back(std::move(session));
    }
    std::sort(out.begin(), out.end(), [](const SessionRecord& a, const SessionRecord& b) {
        return a.started_at > b.started_at;
    });
    return out;
}

bool FileRuntimeStore::claim_resource(const ResourceClaim& claim) {
    if (claim.session_id.empty() || claim.resource_type.empty() || claim.resource_name.empty()) {
        return false;
    }
    std::lock_guard lock(mutex_);
    if (!ensure_ready_unlocked()) {
        return false;
    }
    auto root = load_object_file(claims_path());
    ResourceClaim stored = claim;
    if (stored.claimed_at <= 0) {
        stored.claimed_at = now_epoch_seconds();
    }
    stored.released_at = 0;
    root[claim_key(stored.resource_type, stored.resource_name)] = claim_to_json(stored);
    return save_object_file(claims_path(), root);
}

bool FileRuntimeStore::release_resource(
    const std::string& session_id,
    const std::string& resource_type,
    const std::string& resource_name) {
    if (session_id.empty() || resource_type.empty() || resource_name.empty()) {
        return false;
    }
    std::lock_guard lock(mutex_);
    if (!ensure_ready_unlocked()) {
        return false;
    }
    auto root = load_object_file(claims_path());
    const auto key = claim_key(resource_type, resource_name);
    if (!root.contains(key)) {
        return true;
    }
    auto claim = claim_from_json(root.at(key));
    if (claim.session_id != session_id) {
        return false;
    }
    if (claim.released_at == 0) {
        claim.released_at = now_epoch_seconds();
        root[key] = claim_to_json(claim);
        return save_object_file(claims_path(), root);
    }
    return true;
}

bool FileRuntimeStore::release_session_resources(const std::string& session_id) {
    if (session_id.empty()) {
        return false;
    }
    std::lock_guard lock(mutex_);
    if (!ensure_ready_unlocked()) {
        return false;
    }
    auto root = load_object_file(claims_path());
    const auto now = now_epoch_seconds();
    bool changed = false;
    for (auto it = root.begin(); it != root.end(); ++it) {
        auto claim = claim_from_json(it.value());
        if (claim.session_id != session_id || claim.released_at != 0) {
            continue;
        }
        claim.released_at = now;
        it.value() = claim_to_json(claim);
        changed = true;
    }
    if (!changed) {
        return true;
    }
    return save_object_file(claims_path(), root);
}

std::vector<ResourceClaim> FileRuntimeStore::list_claims(bool held_only) {
    std::lock_guard lock(mutex_);
    const auto root = load_object_file(claims_path());
    std::vector<ResourceClaim> out;
    for (auto it = root.begin(); it != root.end(); ++it) {
        auto claim = claim_from_json(it.value());
        if (held_only && claim.released_at != 0) {
            continue;
        }
        out.push_back(std::move(claim));
    }
    std::sort(out.begin(), out.end(), [](const ResourceClaim& a, const ResourceClaim& b) {
        if (a.resource_type != b.resource_type) {
            return a.resource_type < b.resource_type;
        }
        return a.resource_name < b.resource_name;
    });
    return out;
}

std::optional<ResourceClaim> FileRuntimeStore::find_held_resource(
    const std::string& resource_type,
    const std::string& resource_name) {
    if (resource_type.empty() || resource_name.empty()) {
        return std::nullopt;
    }
    std::lock_guard lock(mutex_);
    const auto root = load_object_file(claims_path());
    const auto key = claim_key(resource_type, resource_name);
    if (!root.contains(key)) {
        return std::nullopt;
    }
    auto claim = claim_from_json(root.at(key));
    if (claim.released_at != 0) {
        return std::nullopt;
    }
    return claim;
}

bool FileRuntimeStore::record_event(const RuntimeEvent& event) {
    RuntimeEvent stored = event;
    if (stored.timestamp <= 0) {
        stored.timestamp = now_epoch_seconds();
    }
    const auto day = day_string_from_epoch(stored.timestamp);
    const auto line = event_to_json(stored).dump();
    {
        std::lock_guard lock(mutex_);
        if (!ensure_ready_unlocked()) {
            return false;
        }
        std::ofstream out(events_path(day), std::ios::app);
        if (!out) {
            return false;
        }
        out << line << '\n';
        if (!out) {
            return false;
        }
    }
    std::cerr << "cadence: " << stored.kind;
    if (!stored.username.empty()) {
        std::cerr << " user=" << stored.username;
    }
    if (!stored.game_key.empty()) {
        std::cerr << " game=" << stored.game_key;
    }
    if (stored.slot >= 0) {
        std::cerr << " slot=" << stored.slot;
    }
    if (!stored.detail.empty()) {
        std::cerr << " " << stored.detail;
    }
    std::cerr << '\n';
    return true;
}

std::vector<RuntimeEvent> FileRuntimeStore::recent_events(
    const std::string& day,
    std::size_t limit) {
    const auto use_day = day.empty() ? day_string_from_epoch(now_epoch_seconds()) : day;
    std::lock_guard lock(mutex_);
    std::ifstream in(events_path(use_day));
    if (!in) {
        return {};
    }
    std::vector<RuntimeEvent> all;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) {
            continue;
        }
        try {
            all.push_back(event_from_json(nlohmann::json::parse(line)));
        } catch (const nlohmann::json::exception&) {
        }
    }
    if (limit == 0 || all.size() <= limit) {
        // Newest last in file; reverse for newest-first.
        std::reverse(all.begin(), all.end());
        return all;
    }
    std::vector<RuntimeEvent> out(
        all.end() - static_cast<std::ptrdiff_t>(limit), all.end());
    std::reverse(out.begin(), out.end());
    return out;
}

} // namespace archstreamer::cadence
