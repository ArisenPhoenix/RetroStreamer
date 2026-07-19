#pragma once

#include "common/protocol.hpp"

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace archstreamer {

struct ClientSeatRequest {
    ClientId client_id = 0;
    std::uint8_t requested_players = 0;
};

class SeatManager {
public:
    void set_host_player_count(std::uint8_t player_count) {
        if (player_count > 1) {
            throw std::runtime_error("host supports at most one local player");
        }
        host_player_count_ = player_count;
    }

    std::uint8_t host_player_count() const {
        return host_player_count_;
    }

    SeatAssignment assign(const std::vector<ClientSeatRequest>& remote_requests) const {
        SeatAssignment assignment;
        RetroArchPort next_port = 0;

        if (host_player_count_ == 1) {
            assignment.seats.push_back(PlayerSeat{HostClientId, 0, 0});
            ++next_port;
        }

        for (const auto& request : remote_requests) {
            if (!valid_player_count(request.requested_players)) {
                throw std::runtime_error("client requested too many players");
            }

            for (LocalPlayerIndex local_player = 0; local_player < request.requested_players; ++local_player) {
                if (next_port >= MaxRetroArchPorts) {
                    throw std::runtime_error("not enough RetroArch ports for requested players");
                }

                assignment.seats.push_back(PlayerSeat{
                    request.client_id,
                    local_player,
                    next_port,
                });
                ++next_port;
            }
        }

        return assignment;
    }

private:
    std::uint8_t host_player_count_ = 0;
};

inline std::optional<RetroArchPort> find_retroarch_port(
    const SeatAssignment& assignment,
    ClientId client_id,
    LocalPlayerIndex local_player) {
    const auto it = std::find_if(assignment.seats.begin(), assignment.seats.end(), [&](const PlayerSeat& seat) {
        return seat.client_id == client_id && seat.local_player == local_player;
    });

    if (it == assignment.seats.end()) {
        return std::nullopt;
    }

    return it->retroarch_port;
}

} // namespace archstreamer
