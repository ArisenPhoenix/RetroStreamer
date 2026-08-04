#pragma once
#include "common/protocol.hpp"

namespace archstreamer {

class InputSender {
public:
    explicit InputSender(ClientId client_id);
    ControllerInput make_input(LocalPlayerIndex local_player, ControllerState state) const;
    KeyboardInput make_keyboard(LocalPlayerIndex local_player, KeyboardState state) const;
    /** @p norm_x/@p norm_y are 0..65535 within the DS bottom screen. */
    TouchInput make_touch(
        LocalPlayerIndex local_player,
        std::uint16_t norm_x,
        std::uint16_t norm_y,
        bool pressed) const;

private:
    ClientId client_id_;
};

} // namespace archstreamer
