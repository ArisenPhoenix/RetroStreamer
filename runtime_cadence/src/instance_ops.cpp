#include "archstreamer/runtime_cadence/instance_ops.hpp"

#include "archstreamer/runtime_cadence/types.hpp"

#include <cerrno>
#include <cstdlib>
#include <unordered_set>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <signal.h>
#include <unistd.h>
#endif

namespace archstreamer::cadence {
namespace {

bool parse_host_pid(std::string_view host_id, long& out_pid) {
    if (host_id.empty()) {
        return false;
    }
    char* end = nullptr;
    const auto pid = std::strtol(std::string(host_id).c_str(), &end, 10);
    if (end == nullptr || *end != '\0' || pid <= 0) {
        return false;
    }
    out_pid = pid;
    return true;
}

void record_reap_event(
    RuntimeStore& store,
    std::string_view kind,
    const SessionRecord* session,
    std::string_view host_id,
    int slot,
    std::string_view detail) {
    RuntimeEvent event;
    event.kind = std::string(kind);
    event.host_id = std::string(host_id);
    event.slot = slot;
    event.detail = std::string(detail);
    if (session != nullptr) {
        event.username = session->username;
        event.game_key = session->game_key;
        event.session_id = session->session_id;
        if (event.slot < 0) {
            event.slot = session->slot;
        }
    }
    (void)store.record_event(event);
}

StaleReapResult end_session_and_claims(
    RuntimeStore& store,
    const SessionRecord& session,
    std::string_view reason) {
    StaleReapResult result;
    const auto before = store.list_claims(true);
    std::size_t held_for_session = 0;
    for (const auto& claim : before) {
        if (claim.session_id == session.session_id) {
            ++held_for_session;
        }
    }
    (void)store.release_session_resources(session.session_id);
    if (store.end_session(session.session_id, std::string(reason))) {
        ++result.sessions_ended;
    }
    result.claims_released = held_for_session;
    record_reap_event(
        store,
        "session_ended",
        &session,
        session.host_id,
        session.slot,
        reason);
    if (held_for_session > 0) {
        record_reap_event(
            store,
            "resources_released",
            &session,
            session.host_id,
            session.slot,
            reason);
    }
    return result;
}

} // namespace

bool host_process_alive(std::string_view host_id) {
    long pid = 0;
    if (!parse_host_pid(host_id, pid)) {
        return false;
    }
#if defined(_WIN32)
    HANDLE handle = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (handle == nullptr) {
        return false;
    }
    DWORD code = 0;
    const bool ok = ::GetExitCodeProcess(handle, &code) != 0;
    ::CloseHandle(handle);
    return ok && code == STILL_ACTIVE;
#else
    if (::kill(static_cast<pid_t>(pid), 0) == 0) {
        return true;
    }
    // Exists but we lack permission — still a live process.
    return errno == EPERM;
#endif
}

StaleReapResult reap_stale_instance_state(
    RuntimeStore& store,
    std::string_view current_host_id) {
    StaleReapResult total;
    if (!store.ensure_ready()) {
        return total;
    }

    const auto active = store.list_sessions(true);
    std::unordered_set<std::string> live_session_ids;
    live_session_ids.reserve(active.size());

    for (const auto& session : active) {
        if (session.session_id.empty()) {
            continue;
        }
        const bool is_current = !current_host_id.empty() &&
            session.host_id == current_host_id;
        if (is_current || host_process_alive(session.host_id)) {
            live_session_ids.insert(session.session_id);
            continue;
        }
        const auto part = end_session_and_claims(store, session, "stale host");
        total.sessions_ended += part.sessions_ended;
        total.claims_released += part.claims_released;
    }

    // Stale Connected rows for dead host processes.
    for (const auto& connection : store.list_connections(true)) {
        const bool is_current = !current_host_id.empty() &&
            connection.host_id == current_host_id;
        if (is_current || host_process_alive(connection.host_id)) {
            continue;
        }
        if (store.end_connection(connection.connection_id, "stale host")) {
            ++total.connections_ended;
        }
    }

    // Orphan held claims: session gone/ended, or host process dead.
    const auto held = store.list_claims(true);
    for (const auto& claim : held) {
        if (live_session_ids.contains(claim.session_id)) {
            continue;
        }
        bool stale = false;
        std::string reason = "orphan claim";
        if (const auto session = store.find_session(claim.session_id); !session.has_value()) {
            stale = true;
            reason = "orphan claim (missing session)";
        } else if (session->ended_at != 0) {
            stale = true;
            reason = "orphan claim (ended session)";
        } else if (
            (current_host_id.empty() || claim.host_id != current_host_id) &&
            !host_process_alive(claim.host_id)) {
            stale = true;
            reason = "stale host";
        }
        if (!stale) {
            // Session still active for a live host we didn't end above.
            live_session_ids.insert(claim.session_id);
            continue;
        }
        if (store.release_resource(claim.session_id, claim.resource_type, claim.resource_name)) {
            ++total.claims_released;
            RuntimeEvent event;
            event.kind = "resource_released";
            event.host_id = claim.host_id;
            event.slot = claim.slot;
            event.session_id = claim.session_id;
            event.detail = std::string(claim.resource_type) + "=" + claim.resource_name +
                " " + reason;
            (void)store.record_event(event);
        }
    }

    return total;
}

StaleReapResult release_host_instance_state(
    RuntimeStore& store,
    std::string_view host_id,
    std::string_view reason) {
    StaleReapResult total;
    if (host_id.empty() || !store.ensure_ready()) {
        return total;
    }
    for (const auto& session : store.list_sessions(true)) {
        if (session.host_id != host_id) {
            continue;
        }
        const auto part = end_session_and_claims(store, session, reason);
        total.sessions_ended += part.sessions_ended;
        total.claims_released += part.claims_released;
    }
    std::size_t live_connections = 0;
    for (const auto& connection : store.list_connections(true)) {
        if (connection.host_id == host_id) {
            ++live_connections;
        }
    }
    if (live_connections > 0 &&
        store.end_connections_for_host(std::string(host_id), std::string(reason))) {
        total.connections_ended += live_connections;
    }
    // Any leftover held claims tagged with this host_id.
    for (const auto& claim : store.list_claims(true)) {
        if (claim.host_id != host_id) {
            continue;
        }
        if (store.release_resource(claim.session_id, claim.resource_type, claim.resource_name)) {
            ++total.claims_released;
        }
    }
    RuntimeEvent stopped;
    stopped.kind = "host_stopped";
    stopped.host_id = std::string(host_id);
    stopped.detail = std::string(reason);
    (void)store.record_event(stopped);
    return total;
}

} // namespace archstreamer::cadence
