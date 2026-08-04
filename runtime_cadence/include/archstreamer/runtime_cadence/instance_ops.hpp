#pragma once

#include "archstreamer/runtime_cadence/store.hpp"

#include <cstddef>
#include <string>
#include <string_view>

namespace archstreamer::cadence {

struct StaleReapResult {
    std::size_t sessions_ended = 0;
    std::size_t claims_released = 0;
};

/**
 * True if `host_id` (decimal PID string) still names a live process.
 * Non-numeric / empty → false. EPERM on kill(0) counts as alive.
 */
bool host_process_alive(std::string_view host_id);

/**
 * End active sessions and release held claims whose host_id process is dead
 * (and is not `current_host_id`). Also drops orphan held claims whose session
 * is missing or already ended. Records control-plane events.
 */
StaleReapResult reap_stale_instance_state(
    RuntimeStore& store,
    std::string_view current_host_id);

/**
 * End every still-active session for `host_id` and release its claims.
 * Used on clean host shutdown.
 */
StaleReapResult release_host_instance_state(
    RuntimeStore& store,
    std::string_view host_id,
    std::string_view reason);

} // namespace archstreamer::cadence
