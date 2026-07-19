#include "client/input_sender.hpp"

#include "common/time.hpp"

#include <stdexcept>

namespace archstreamer {

InputSender::InputSender(ClientId client_id) : client_id_(client_id) {
}

ControllerInput InputSender::make_input(LocalPlayerIndex local_player, ControllerState state) const {
    if (local_player >= MaxPlayersPerClient) {
        throw std::runtime_error("invalid local player index");
    }

    if (state.timestamp_us == 0) {
        state.timestamp_us = steady_timestamp_us();
    }

    return ControllerInput{
        client_id_,
        local_player,
        state,
    };
}

} // namespace archstreamer
