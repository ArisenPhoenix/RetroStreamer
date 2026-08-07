#pragma once

#include "common/protocol.hpp"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
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
 * Tracks mutual link requests between seated clients (host-wide username bond).
 * Game id is not a match gate. HostSessionHub arms LinkCableBackend only when
 * both peers share the same system_key.
 */
class LinkCoordinator {
public:
    /**
     * @param target_seated If set, used instead of plan-local username lookup
     *        (host-wide concurrent SP slots).
     * @param peer_connected If set, decides whether to emit the peer Matched reply
     *        (cross-slot peer may live in another SessionPlan).
     */
    std::vector<LinkOutbound> handle(
        const SessionPlan& plan,
        ClientId from_client_id,
        const std::string& from_username,
        const LinkRequest& request,
        std::function<bool(std::string_view)> target_seated = {},
        std::function<bool(ClientId)> peer_connected = {});

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
        const std::string& target_username) const;
};

} // namespace archstreamer
