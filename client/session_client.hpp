#pragma once

#include "../common/protocol.hpp"

#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace archstreamer {

class SessionClient {
public:
    ClientHello make_hello(
        std::string display_name,
        std::uint8_t requested_players,
        bool wants_video = true,
        bool wants_audio = true) const {
        if (!valid_player_count(requested_players)) {
            throw std::runtime_error("invalid requested player count");
        }

        return ClientHello{
            std::move(display_name),
            requested_players,
            wants_video,
            wants_audio,
        };
    }

    ClientConfig make_config(
        std::uint8_t requested_players,
        bool wants_video = true,
        bool wants_audio = true) const {
        if (!valid_player_count(requested_players)) {
            throw std::runtime_error("invalid requested player count");
        }

        return ClientConfig{
            requested_players,
            wants_video,
            wants_audio,
        };
    }

    void apply_welcome(const HostWelcome& welcome) {
        client_id_ = welcome.client_id;
    }

    void apply_seats(SeatAssignment seats) {
        seats_ = std::move(seats);
    }

    std::optional<ClientId> client_id() const {
        return client_id_;
    }

    const SeatAssignment& seats() const {
        return seats_;
    }

private:
    std::optional<ClientId> client_id_;
    SeatAssignment seats_;
};

} // namespace archstreamer
