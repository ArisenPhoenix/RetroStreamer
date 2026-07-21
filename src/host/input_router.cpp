#include "host/input_router.hpp"

#include <utility>

namespace archstreamer {

InputRouter::InputRouter(VirtualGamepadBus& gamepads) : gamepads_(gamepads) {
}

void InputRouter::set_seat_assignment(SeatAssignment assignment) {
    assignment_ = std::move(assignment);
    last_input_timestamp_by_player_.clear();
}

bool InputRouter::route(const ControllerInput& input) {
    const auto port = find_retroarch_port(assignment_, input.client_id, input.local_player);
    if (!port.has_value()) {
        return false;
    }

    const PlayerKey key{input.client_id, input.local_player};
    const auto last_timestamp = last_input_timestamp_by_player_.find(key);
    if (last_timestamp != last_input_timestamp_by_player_.end() &&
        input.state.timestamp_us <= last_timestamp->second) {
        return false;
    }

    last_input_timestamp_by_player_[key] = input.state.timestamp_us;
    gamepads_.update(*port, input.state);
    return true;
}

void InputRouter::neutralize_client(ClientId client_id) {
    for (const auto& seat : assignment_.seats) {
        if (seat.client_id != client_id) {
            continue;
        }

        auto neutral = ControllerState{};
        const PlayerKey key{seat.client_id, seat.local_player};
        const auto last_timestamp = last_input_timestamp_by_player_.find(key);
        if (last_timestamp != last_input_timestamp_by_player_.end()) {
            neutral.timestamp_us = last_timestamp->second + 1;
            last_timestamp->second = neutral.timestamp_us;
        }
        gamepads_.update(seat.retroarch_port, neutral);
    }
}

bool InputRouter::PlayerKey::operator<(const PlayerKey& other) const {
    if (client_id != other.client_id) {
        return client_id < other.client_id;
    }

    return local_player < other.local_player;
}

} // namespace archstreamer
