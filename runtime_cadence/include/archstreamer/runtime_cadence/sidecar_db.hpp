#pragma once

#include "archstreamer/runtime_cadence/types.hpp"

#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

struct sqlite3;

namespace archstreamer::cadence {

class SidecarDb {
public:
    explicit SidecarDb(std::filesystem::path db_path);
    ~SidecarDb();

    SidecarDb(const SidecarDb&) = delete;
    SidecarDb& operator=(const SidecarDb&) = delete;

    bool open();
    bool upsert_user(const UserRecord& user);
    std::optional<UserRecord> find_user(const std::string& username);
    bool delete_user(const std::string& username);
    std::vector<UserRecord> list_users();

    bool upsert_session(const SessionRecord& session);
    bool end_session(const std::string& session_id, const std::string& end_reason);
    std::optional<SessionRecord> find_session(const std::string& session_id);
    std::vector<SessionRecord> list_sessions(bool active_only);

    bool claim_resource(const ResourceClaim& claim);
    bool release_resource(
        const std::string& session_id,
        const std::string& resource_type,
        const std::string& resource_name);
    bool release_session_resources(const std::string& session_id);
    std::vector<ResourceClaim> list_claims(bool held_only);
    std::optional<ResourceClaim> find_held_resource(
        const std::string& resource_type,
        const std::string& resource_name);

    bool record_event(const RuntimeEvent& event);
    std::vector<RuntimeEvent> recent_events(const std::string& day, std::size_t limit);

    /** Handle one JSON request object; returns response JSON object. */
    std::string handle_request_json(const std::string& request_json);

private:
    bool exec(const char* sql);
    bool exec_quiet(const char* sql);
    bool ensure_schema();
    bool ensure_events_table(const std::string& day);
    static std::string events_table_name(const std::string& day);

    std::filesystem::path db_path_;
    sqlite3* db_ = nullptr;
    std::mutex mutex_;
};

} // namespace archstreamer::cadence
