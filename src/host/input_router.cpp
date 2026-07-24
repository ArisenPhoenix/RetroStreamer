#include "host/input_router.hpp"

#include <iostream>
#include <utility>

namespace archstreamer {

InputRouter::InputRouter(VirtualGamepadBus& gamepads) : gamepads_(gamepads) {
}

void InputRouter::set_seat_assignment(SeatAssignment assignment) {
    std::lock_guard lock(mutex_);
    assignment_ = std::move(assignment);
    last_input_timestamp_by_player_.clear();
}

bool InputRouter::route(const ControllerInput& input) {
    std::lock_guard lock(mutex_);
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
    if (!first_input_logged_) {
        first_input_logged_ = true;
        std::cout
            << "First controller input applied: client " << static_cast<int>(input.client_id)
            << " local P" << static_cast<int>(input.local_player) + 1
            << " -> RetroArch P" << static_cast<int>(*port) + 1 << '\n';
    }
    return true;
}

void InputRouter::neutralize_client(ClientId client_id) {
    std::lock_guard lock(mutex_);
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
