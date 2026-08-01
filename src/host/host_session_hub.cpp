#include "host/host_session_hub.hpp"

#include "host/active_session_slot.hpp"
#include "host/host_session_helpers.hpp"
#include "host/retroarch_netcmd.hpp"
#include "host/session_lobby.hpp"

#include <algorithm>
#include <cctype>
#include <iostream>

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

} // namespace

ClientId HostSessionHub::allocate_client_id() {
    std::lock_guard lock(mutex_);
    return next_client_id_++;
}

void HostSessionHub::register_slot(ActiveSessionSlot* slot) {
    std::lock_guard lock(mutex_);
    if (slot == nullptr) {
        return;
    }
    if (std::find(slots_.begin(), slots_.end(), slot) == slots_.end()) {
        slots_.push_back(slot);
    }
}

void HostSessionHub::unregister_slot(ActiveSessionSlot* slot) {
    std::lock_guard lock(mutex_);
    slots_.erase(std::remove(slots_.begin(), slots_.end(), slot), slots_.end());
}

bool HostSessionHub::username_seated(std::string_view username, const GameId& game_id) const {
    std::lock_guard lock(mutex_);
    for (const auto* slot : slots_) {
        if (slot == nullptr || slot->finished()) {
            continue;
        }
        const auto& plan = slot->plan();
        if (!game_id.empty() && plan.selected_game_id != game_id) {
            continue;
        }
        for (const auto& client : plan.clients) {
            if (client.connection_state != SessionConnectionState::Connected) {
                continue;
            }
            if (client.hello.requested_players == 0) {
                continue;
            }
            if (username_equal(client.hello.username, username)) {
                return true;
            }
        }
    }
    return false;
}

bool HostSessionHub::save_profile_active(std::string_view username) const {
    std::lock_guard lock(mutex_);
    for (const auto* slot : slots_) {
        if (slot == nullptr || slot->finished()) {
            continue;
        }
        if (username_equal(slot->plan().save_username, username)) {
            return true;
        }
    }
    return false;
}

ActiveSessionSlot* HostSessionHub::slot_for_client(ClientId client_id) {
    std::lock_guard lock(mutex_);
    for (auto* slot : slots_) {
        if (slot == nullptr || slot->finished()) {
            continue;
        }
        for (const auto& client : slot->plan().clients) {
            if (client.client_id == client_id) {
                return slot;
            }
        }
    }
    return nullptr;
}

const ActiveSessionSlot* HostSessionHub::slot_for_client(ClientId client_id) const {
    return const_cast<HostSessionHub*>(this)->slot_for_client(client_id);
}

ActiveSessionSlot* HostSessionHub::slot_for_reconnect(const ClientHello& hello) {
    std::lock_guard lock(mutex_);
    for (auto* slot : slots_) {
        if (slot == nullptr || slot->finished()) {
            continue;
        }
        if (!hello.selected_game_id.has_value() ||
            slot->plan().selected_game_id != *hello.selected_game_id) {
            continue;
        }
        if (slot->plan().session_mode != hello.session_mode) {
            continue;
        }
        if (disconnected_player_for_reconnect(slot->plan(), hello) != nullptr) {
            return slot;
        }
    }
    return nullptr;
}

ActiveSessionSlot* HostSessionHub::slot_for_late_viewer(const ClientHello& hello) {
    std::lock_guard lock(mutex_);
    for (auto* slot : slots_) {
        if (slot == nullptr || slot->finished()) {
            continue;
        }
        if (!hello.selected_game_id.has_value() ||
            slot->plan().selected_game_id != *hello.selected_game_id) {
            continue;
        }
        if (slot->plan().session_mode != hello.session_mode) {
            continue;
        }
        return slot;
    }
    return nullptr;
}

std::size_t HostSessionHub::live_slot_count() const {
    std::lock_guard lock(mutex_);
    std::size_t count = 0;
    for (const auto* slot : slots_) {
        if (slot != nullptr && !slot->finished()) {
            ++count;
        }
    }
    return count;
}

bool HostSessionHub::has_multiplayer_slot() const {
    std::lock_guard lock(mutex_);
    for (const auto* slot : slots_) {
        if (slot != nullptr && !slot->finished() && slot->is_multiplayer()) {
            return true;
        }
    }
    return false;
}

bool HostSessionHub::has_singleplayer_slot() const {
    std::lock_guard lock(mutex_);
    for (const auto* slot : slots_) {
        if (slot != nullptr && !slot->finished() && !slot->is_multiplayer()) {
            return true;
        }
    }
    return false;
}

std::vector<ActiveSessionSlot*> HostSessionHub::slots() {
    std::lock_guard lock(mutex_);
    return slots_;
}

void HostSessionHub::clear_link_client(ClientId client_id) {
    link_coordinator_.clear_client(client_id);
    if (client_id == link_cable_.client_a() || client_id == link_cable_.client_b()) {
        link_cable_.clear();
    }
}

std::vector<LinkOutbound> HostSessionHub::handle_link(
    ActiveSessionSlot& from_slot,
    ClientId from_client_id,
    const std::string& from_username,
    const LinkRequest& request) {
    const GameId& game_id = from_slot.plan().selected_game_id;
    auto outbound = link_coordinator_.handle(
        from_slot.plan(),
        from_client_id,
        from_username,
        request,
        [this, &game_id](std::string_view target) {
            return username_seated(target, game_id);
        },
        [this](ClientId peer_id) {
            return slot_for_client(peer_id) != nullptr;
        });

    bool started_cable = false;
    for (auto& item : outbound) {
        if (!started_cable && item.response.status == LinkStatus::Matched) {
            started_cable = true;
            std::string peer_user = item.response.peer_username;
            ClientId peer_id = 0;
            for (const auto& other : outbound) {
                if (other.client_id != item.client_id) {
                    peer_id = other.client_id;
                    break;
                }
            }
            if (peer_id == 0) {
                // Resolve peer by username across slots.
                for (auto* slot : slots()) {
                    if (slot == nullptr || slot->finished()) {
                        continue;
                    }
                    for (const auto& candidate : slot->plan().clients) {
                        if (candidate.client_id != from_client_id &&
                            candidate.connection_state == SessionConnectionState::Connected &&
                            username_equal(candidate.hello.username, peer_user)) {
                            peer_id = candidate.client_id;
                            break;
                        }
                    }
                    if (peer_id != 0) {
                        break;
                    }
                }
            }

            ActiveSessionSlot* peer_slot = slot_for_client(peer_id);
            const bool cross_slot =
                peer_slot != nullptr && peer_slot != &from_slot;

            const std::uint16_t netplay_port = static_cast<std::uint16_t>(
                55435 +
                std::min(from_slot.slot_index(), peer_slot != nullptr ? peer_slot->slot_index() : 0));

            const auto start = link_cable_.begin(
                from_slot.plan().system_key,
                peer_id,
                from_client_id,
                peer_user,
                from_username,
                2,
                cross_slot,
                netplay_port);

            for (auto& update : outbound) {
                update.response.message = start.message;
                if (!start.ok) {
                    update.response.ok = false;
                    update.response.status = LinkStatus::Error;
                }
            }

            if (start.ok) {
                std::cout << "Link cable: " << start.message << '\n';
                // Cross-slot: both emulators already running — no promote.
                // Same-slot multi: keep existing promote flag path on that plan.
                if (start.needs_runtime_promotion && !cross_slot) {
                    from_slot.plan().pending_link_promotion = true;
                    from_slot.plan().pending_link_host_client_id = start.logical_host_client_id;
                    from_slot.plan().pending_link_client_client_id = start.logical_client_client_id;
                    from_slot.plan().pending_link_host_username = start.logical_host_username;
                    from_slot.plan().pending_link_client_username = start.logical_client_username;
                }
                if (start.needs_gba_netplay && cross_slot && peer_slot != nullptr) {
                    ActiveSessionSlot* host_slot =
                        slot_for_client(start.logical_host_client_id);
                    ActiveSessionSlot* client_slot =
                        slot_for_client(start.logical_client_client_id);
                    if (host_slot == nullptr || client_slot == nullptr) {
                        std::cerr
                            << "Link cable: GBA netplay matched but could not resolve both slots\n";
                    } else {
                        GbaNetplayRelaunchRequest host_req;
                        host_req.is_host = true;
                        host_req.port = start.netplay_port;
                        host_req.nick = start.logical_host_username;
                        host_req.core_path = start.core_path;
                        GbaNetplayRelaunchRequest client_req;
                        client_req.is_host = false;
                        client_req.port = start.netplay_port;
                        client_req.nick = start.logical_client_username;
                        client_req.core_path = start.core_path;
                        // Host first so its pending is visible before client delay ends.
                        host_slot->request_gba_netplay_relaunch(std::move(host_req));
                        client_slot->request_gba_netplay_relaunch(std::move(client_req));
                        std::cout
                            << "Link cable: queued gpSP netplay "
                            << "host_slot=" << host_slot->slot_index()
                            << " client_slot=" << client_slot->slot_index()
                            << " port=" << start.netplay_port << '\n';
                    }
                }
                send_retroarch_netcmd(
                    std::string("SHOW_MSG ") + start.message,
                    from_slot.plan().retroarch_netcmd_port);
                if (cross_slot && peer_slot != nullptr) {
                    send_retroarch_netcmd(
                        std::string("SHOW_MSG ") + start.message,
                        peer_slot->plan().retroarch_netcmd_port);
                }
            } else {
                std::cerr << "Link cable: " << start.message << '\n';
            }
        }
    }

    return outbound;
}

} // namespace archstreamer
