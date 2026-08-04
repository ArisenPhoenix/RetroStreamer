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

KeyboardInput InputSender::make_keyboard(LocalPlayerIndex local_player, KeyboardState state) const {
    if (local_player >= MaxPlayersPerClient) {
        throw std::runtime_error("invalid local player index");
    }

    if (state.timestamp_us == 0) {
        state.timestamp_us = steady_timestamp_us();
    }

    return KeyboardInput{
        client_id_,
        local_player,
        state,
    };
}

TouchInput InputSender::make_touch(
    LocalPlayerIndex local_player,
    std::uint16_t norm_x,
    std::uint16_t norm_y,
    bool pressed) const {
    if (local_player >= MaxPlayersPerClient) {
        throw std::runtime_error("invalid local player index");
    }

    return TouchInput{
        client_id_,
        local_player,
        0,
        steady_timestamp_us(),
        norm_x,
        norm_y,
        pressed,
    };
}

} // namespace archstreamer
