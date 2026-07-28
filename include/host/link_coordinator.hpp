#pragma once

#include "common/protocol.hpp"

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace archstreamer {

struct SessionPlan;

/** Outbound reply produced by LinkCoordinator (may notify both peers on match). */
struct LinkOutbound {
    ClientId client_id = 0;
    LinkResponse response;
};

/**
 * Tracks mutual link requests between seated clients.
 * On match, LinkCableBackend may start a system-specific backend (multi-instance TBD;
 * DEBUG_GB_LINK enables experimental DoubleCherryGB dual-GB relaunch).
 */
class LinkCoordinator {
public:
    std::vector<LinkOutbound> handle(
        const SessionPlan& plan,
        ClientId from_client_id,
        const std::string& from_username,
        const LinkRequest& request);

    void clear_client(ClientId client_id);

private:
    struct PendingOffer {
        ClientId from_client_id = 0;
        std::string from_username;
        std::string target_username;
        GameId game_id;
    };

    std::vector<PendingOffer> pending_;

    void erase_from(ClientId client_id);
    const PendingOffer* find_mutual(
        ClientId from_client_id,
        const std::string& from_username,
        const std::string& target_username,
        const GameId& game_id) const;
};

} // namespace archstreamer
