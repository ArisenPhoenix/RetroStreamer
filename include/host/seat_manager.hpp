#pragma once

#include "common/protocol.hpp"

#include <cstdint>
#include <optional>
#include <vector>

namespace archstreamer {

struct ClientSeatRequest {
    ClientId client_id = 0;
    std::uint8_t requested_players = 0;
};

class SeatManager {
public:
    void set_host_player_count(std::uint8_t player_count);
    std::uint8_t host_player_count() const;
    SeatAssignment assign(const std::vector<ClientSeatRequest>& remote_requests) const;

private:
    std::uint8_t host_player_count_ = 0;
};

std::optional<RetroArchPort> find_retroarch_port(
    const SeatAssignment& assignment,
    ClientId client_id,
    LocalPlayerIndex local_player);

} // namespace archstreamer
