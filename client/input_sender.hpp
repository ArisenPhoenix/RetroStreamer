#pragma once

#include "../common/protocol.hpp"

#include <optional>
#include <stdexcept>

namespace archstreamer {

class InputSender {
public:
    explicit InputSender(ClientId client_id) : client_id_(client_id) {}

    ControllerInput make_input(LocalPlayerIndex local_player, ControllerState state) const {
        if (local_player >= MaxPlayersPerClient) {
            throw std::runtime_error("invalid local player index");
        }

        return ControllerInput{
            client_id_,
            local_player,
            state,
        };
    }

private:
    ClientId client_id_;
};

} // namespace archstreamer
