#pragma once

#include "archstreamer/runtime_cadence/store.hpp"

#include <filesystem>
#include <mutex>
#include <string>

namespace archstreamer::cadence {

/**
 * Client for the archstreamer_cadence sidecar (Unix socket + JSON).
 * On platforms without the sidecar, ensure_ready fails soft and methods no-op.
 */
class DbRuntimeStore final : public RuntimeStore {
public:
    explicit DbRuntimeStore(
        std::filesystem::path socket_path = {},
        std::filesystem::path sidecar_path = {});
    ~DbRuntimeStore() override;

    bool ensure_ready() override;
    bool upsert_user(const UserRecord& user) override;
    std::optional<UserRecord> find_user(const std::string& username) override;
    bool delete_user(const std::string& username) override;
    std::vector<UserRecord> list_users() override;

    bool upsert_session(const SessionRecord& session) override;
    bool end_session(const std::string& session_id, const std::string& end_reason) override;
    std::optional<SessionRecord> find_session(const std::string& session_id) override;
    std::vector<SessionRecord> list_sessions(bool active_only) override;

    bool claim_resource(const ResourceClaim& claim) override;
    bool release_resource(
        const std::string& session_id,
        const std::string& resource_type,
        const std::string& resource_name) override;
    bool release_session_resources(const std::string& session_id) override;
    std::vector<ResourceClaim> list_claims(bool held_only) override;
    std::optional<ResourceClaim> find_held_resource(
        const std::string& resource_type,
        const std::string& resource_name) override;

    bool record_event(const RuntimeEvent& event) override;
    std::vector<RuntimeEvent> recent_events(
        const std::string& day,
        std::size_t limit) override;

private:
    bool connect();
    void disconnect();
    bool spawn_sidecar();
    bool request(const std::string& op, const std::string& body_json, std::string& response_json);

    std::filesystem::path socket_path_;
    std::filesystem::path sidecar_path_;
    int fd_ = -1;
    mutable std::mutex mutex_;
};

std::filesystem::path default_cadence_socket_path();
std::filesystem::path default_cadence_sidecar_path();

} // namespace archstreamer::cadence
