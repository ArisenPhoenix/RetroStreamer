#pragma once

#include "archstreamer/runtime_cadence/store.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <mutex>

namespace archstreamer::cadence {

/**
 * Default cadence: users.json + sessions.json + claims.json +
 * events_YYYY-MM-DD.jsonl under the data root.
 */
class FileRuntimeStore final : public RuntimeStore {
public:
    explicit FileRuntimeStore(std::filesystem::path root = {});

    bool ensure_ready() override;
    bool upsert_user(const UserRecord& user) override;
    std::optional<UserRecord> find_user(const std::string& username) override;
    bool delete_user(const std::string& username) override;
    std::vector<UserRecord> list_users() override;

    bool upsert_controls(const ControlsRecord& controls) override;
    std::optional<ControlsRecord> find_controls(
        const std::string& username,
        const std::string& kind) override;
    std::vector<ControlsRecord> list_controls() override;

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

    const std::filesystem::path& root() const { return root_; }

private:
    bool ensure_ready_unlocked();
    std::filesystem::path users_path() const;
    std::filesystem::path controls_path() const;
    std::filesystem::path sessions_path() const;
    std::filesystem::path claims_path() const;
    std::filesystem::path events_path(const std::string& day) const;

    nlohmann::json load_object_file(const std::filesystem::path& path) const;
    bool save_object_file(const std::filesystem::path& path, const nlohmann::json& root) const;
    static std::string claim_key(const std::string& type, const std::string& name);

    std::filesystem::path root_;
    mutable std::mutex mutex_;
};

/** ~/.local/share/archstreamer/cadence (or fallback under cwd). */
std::filesystem::path default_cadence_data_root();

} // namespace archstreamer::cadence
