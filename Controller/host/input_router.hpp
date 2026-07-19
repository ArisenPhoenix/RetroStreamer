#pragma once

#include "seat_manager.hpp"
#include "virtual_gamepad.hpp"

#include <optional>
#include <stdexcept>
#include <utility>

namespace archstreamer {

class InputRouter {
public:
    explicit InputRouter(VirtualGamepadBus& gamepads) : gamepads_(gamepads) {}

    void set_seat_assignment(SeatAssignment assignment) {
        assignment_ = std::move(assignment);
    }

    bool route(const ControllerInput& input) {
        const auto port = find_retroarch_port(assignment_, input.client_id, input.local_player);
        if (!port.has_value()) {
            return false;
        }

        gamepads_.update(*port, input.state);
        return true;
    }

private:
    VirtualGamepadBus& gamepads_;
    SeatAssignment assignment_;
};

} // namespace archstreamer
