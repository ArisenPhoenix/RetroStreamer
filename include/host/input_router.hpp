#pragma once

#include "host/seat_manager.hpp"
#include "host/virtual_gamepad.hpp"

#include <map>

namespace archstreamer {

class InputRouter {
public:
    explicit InputRouter(VirtualGamepadBus& gamepads);

    void set_seat_assignment(SeatAssignment assignment);
    bool route(const ControllerInput& input);
    void neutralize_client(ClientId client_id);

private:
    struct PlayerKey {
        ClientId client_id = 0;
        LocalPlayerIndex local_player = 0;

        bool operator<(const PlayerKey& other) const;
    };

    VirtualGamepadBus& gamepads_;
    SeatAssignment assignment_;
    std::map<PlayerKey, std::uint64_t> last_input_timestamp_by_player_;
    bool first_input_logged_ = false;
};

} // namespace archstreamer
