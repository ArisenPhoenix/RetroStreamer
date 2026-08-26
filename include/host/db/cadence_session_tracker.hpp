#pragma once

#include "archstreamer/runtime_cadence/types.hpp"
#include "host/db/cadence_resource_lease.hpp"

#include <string>
#include <string_view>

namespace archstreamer {

/**
 * Tracks one live session's identity in cadence.
 * Resource names are allocated through leases() — begin() only creates the session row.
 */
class CadenceSessionTracker {
public:
    CadenceSessionTracker() = default;

    [[nodiscard]] const std::string& session_id() const { return session_id_; }
    [[nodiscard]] bool active() const { return !session_id_.empty(); }

    /** Create the session row. Does not pick resource names. */
    void begin(
        int slot,
        std::string_view username,
        std::string_view game_key,
        std::string_view system_key,
        std::string_view mode);

    [[nodiscard]] CadenceResourceLease leases() const;

    void claim(std::string_view resource_type, std::string_view resource_name, std::string_view detail = {});
    void claim_emulator_pid(int pid);

    /** Delete every claim row and mark the session ended. */
    void end(std::string_view end_reason);

private:
    std::string session_id_;
    std::string host_id_;
    int slot_ = -1;
};

} // namespace archstreamer
