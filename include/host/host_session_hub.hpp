#pragma once

#include "common/protocol.hpp"
#include "host/link_cable_backend.hpp"
#include "host/link_coordinator.hpp"

#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace archstreamer {

class ActiveSessionSlot;
struct ClientHello;
struct SessionPlan;

/**
 * Host-wide registry for concurrent session slots: global client ids,
 * cross-slot Link matchmaking, and seated-username lookup.
 */
class HostSessionHub {
public:
    ClientId allocate_client_id();

    void register_slot(ActiveSessionSlot* slot);
    void unregister_slot(ActiveSessionSlot* slot);

    bool username_seated(std::string_view username, const GameId& game_id) const;
    // A persistent save/config profile must have only one live writer. Reconnects
    // attach to that slot before new-session admission reaches this check.
    bool save_profile_active(std::string_view username) const;

    ActiveSessionSlot* slot_for_client(ClientId client_id);
    const ActiveSessionSlot* slot_for_client(ClientId client_id) const;

    ActiveSessionSlot* slot_for_reconnect(const ClientHello& hello);
    ActiveSessionSlot* slot_for_late_viewer(const ClientHello& hello);

    std::size_t live_slot_count() const;
    bool has_multiplayer_slot() const;
    bool has_singleplayer_slot() const;

    std::vector<ActiveSessionSlot*> slots();
    const std::vector<ActiveSessionSlot*>& slots_unsafe() const { return slots_; }

    LinkCoordinator& link_coordinator() { return link_coordinator_; }
    LinkCableBackend& link_cable() { return link_cable_; }

    std::vector<LinkOutbound> handle_link(
        ActiveSessionSlot& from_slot,
        ClientId from_client_id,
        const std::string& from_username,
        const LinkRequest& request);

    void clear_link_client(ClientId client_id);

private:
    mutable std::mutex mutex_;
    ClientId next_client_id_ = 1;
    std::vector<ActiveSessionSlot*> slots_;
    LinkCoordinator link_coordinator_;
    LinkCableBackend link_cable_;
};

} // namespace archstreamer
