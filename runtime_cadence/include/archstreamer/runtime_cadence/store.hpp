#pragma once

#include "archstreamer/runtime_cadence/types.hpp"

#include <optional>
#include <string>
#include <vector>

namespace archstreamer::cadence {

/**
 * Control-plane store: users, sessions/resource claims, runtime events.
 * Implementations must stay off the pad/media session loop.
 * File and db backends expose the same API so every platform uses one path.
 */
class RuntimeStore {
public:
    virtual ~RuntimeStore() = default;

    /** File: create dirs. Db: spawn/attach sidecar if needed. */
    virtual bool ensure_ready() = 0;

    virtual bool upsert_user(const UserRecord& user) = 0;
    virtual std::optional<UserRecord> find_user(const std::string& username) = 0;
    virtual bool delete_user(const std::string& username) = 0;
    virtual std::vector<UserRecord> list_users() = 0;

    virtual bool upsert_session(const SessionRecord& session) = 0;
    virtual bool end_session(const std::string& session_id, const std::string& end_reason) = 0;
    virtual std::optional<SessionRecord> find_session(const std::string& session_id) = 0;
    /** active_only: ended_at == 0. */
    virtual std::vector<SessionRecord> list_sessions(bool active_only) = 0;

    /**
     * Take (or steal) a resource for a session. Clears any prior held claim
     * on the same (type, name) so inventory stays unique.
     */
    virtual bool claim_resource(const ResourceClaim& claim) = 0;
    virtual bool release_resource(
        const std::string& session_id,
        const std::string& resource_type,
        const std::string& resource_name) = 0;
    virtual bool release_session_resources(const std::string& session_id) = 0;
    /** held_only: released_at == 0. */
    virtual std::vector<ResourceClaim> list_claims(bool held_only) = 0;
    virtual std::optional<ResourceClaim> find_held_resource(
        const std::string& resource_type,
        const std::string& resource_name) = 0;

    virtual bool record_event(const RuntimeEvent& event) = 0;

    /**
     * Recent events for `day` (YYYY-MM-DD). Empty day = today.
     * Newest-first when the backend can order them.
     */
    virtual std::vector<RuntimeEvent> recent_events(
        const std::string& day,
        std::size_t limit) = 0;
};

} // namespace archstreamer::cadence
