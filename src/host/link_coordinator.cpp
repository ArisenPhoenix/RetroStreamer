#include "host/link_coordinator.hpp"

#include "host/session_lobby.hpp"

#include <algorithm>

namespace archstreamer {
namespace {

std::string lower_copy(std::string value) {
    for (char& ch : value) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
    return value;
}

bool username_equal(std::string_view a, std::string_view b) {
    return lower_copy(std::string(a)) == lower_copy(std::string(b));
}

bool client_connected(const SessionPlan& plan, ClientId id) {
    for (const auto& client : plan.clients) {
        if (client.client_id == id &&
            client.connection_state == SessionConnectionState::Connected) {
            return true;
        }
    }
    return false;
}

bool target_is_seated(const SessionPlan& plan, std::string_view target_username) {
    for (const auto& client : plan.clients) {
        if (client.connection_state != SessionConnectionState::Connected) {
            continue;
        }
        if (username_equal(client.hello.username, target_username)) {
            return true;
        }
    }
    return false;
}

} // namespace

void LinkCoordinator::erase_from(ClientId client_id) {
    pending_.erase(
        std::remove_if(
            pending_.begin(),
            pending_.end(),
            [client_id](const PendingOffer& offer) {
                return offer.from_client_id == client_id;
            }),
        pending_.end());
}

void LinkCoordinator::clear_client(ClientId client_id) {
    erase_from(client_id);
}

const LinkCoordinator::PendingOffer* LinkCoordinator::find_mutual(
    ClientId from_client_id,
    const std::string& from_username,
    const std::string& target_username,
    const GameId& game_id) const {
    for (const auto& offer : pending_) {
        if (offer.from_client_id == from_client_id) {
            continue;
        }
        if (!username_equal(offer.from_username, target_username)) {
            continue;
        }
        if (!username_equal(offer.target_username, from_username)) {
            continue;
        }
        if (!game_id.empty() && !offer.game_id.empty() && offer.game_id != game_id) {
            continue;
        }
        return &offer;
    }
    return nullptr;
}

std::vector<LinkOutbound> LinkCoordinator::handle(
    const SessionPlan& plan,
    ClientId from_client_id,
    const std::string& from_username,
    const LinkRequest& request) {
    std::vector<LinkOutbound> out;

    LinkResponse base;
    base.peer_username = request.target_username;

    if (!valid_username(from_username)) {
        base.status = LinkStatus::Error;
        base.message = "Invalid local username";
        out.push_back({from_client_id, std::move(base)});
        return out;
    }

    if (request.action == LinkAction::Cancel) {
        erase_from(from_client_id);
        base.ok = true;
        base.status = LinkStatus::Cancelled;
        base.peer_username.clear();
        base.message = "Link request cancelled";
        out.push_back({from_client_id, std::move(base)});
        return out;
    }

    // Request
    if (!valid_username(request.target_username)) {
        base.status = LinkStatus::Error;
        base.message = "Invalid link target username";
        out.push_back({from_client_id, std::move(base)});
        return out;
    }
    if (username_equal(request.target_username, from_username)) {
        base.status = LinkStatus::Error;
        base.message = "Cannot link with yourself";
        out.push_back({from_client_id, std::move(base)});
        return out;
    }
    if (!request.game_id.empty() &&
        !plan.selected_game_id.empty() &&
        request.game_id != plan.selected_game_id) {
        base.status = LinkStatus::Error;
        base.message = "Link game_id does not match the active session";
        out.push_back({from_client_id, std::move(base)});
        return out;
    }
    if (!target_is_seated(plan, request.target_username)) {
        base.status = LinkStatus::Error;
        base.message = "No connected client with that username in this session";
        out.push_back({from_client_id, std::move(base)});
        return out;
    }

    erase_from(from_client_id);
    const auto game_id =
        !request.game_id.empty() ? request.game_id : plan.selected_game_id;

    if (const auto* mutual = find_mutual(
            from_client_id,
            from_username,
            request.target_username,
            game_id);
        mutual != nullptr) {
        const auto peer_id = mutual->from_client_id;
        const auto peer_name = mutual->from_username;
        erase_from(peer_id);

        LinkResponse matched_self;
        matched_self.ok = true;
        matched_self.status = LinkStatus::Matched;
        matched_self.peer_username = peer_name;
        matched_self.message = "Matched with " + peer_name;

        LinkResponse matched_peer;
        matched_peer.ok = true;
        matched_peer.status = LinkStatus::Matched;
        matched_peer.peer_username = from_username;
        matched_peer.message = "Matched with " + from_username;

        out.push_back({from_client_id, std::move(matched_self)});
        if (client_connected(plan, peer_id)) {
            out.push_back({peer_id, std::move(matched_peer)});
        }
        return out;
    }

    pending_.push_back(PendingOffer{
        from_client_id,
        from_username,
        request.target_username,
        game_id,
    });

    base.ok = true;
    base.status = LinkStatus::Pending;
    base.message = "Waiting for " + request.target_username + " to request you back";
    out.push_back({from_client_id, std::move(base)});
    return out;
}

} // namespace archstreamer
