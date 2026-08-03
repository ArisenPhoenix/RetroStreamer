#include "host/input_router.hpp"

#include "common/protocol.hpp"

#include <iostream>
#include <utility>

namespace archstreamer {

InputRouter::InputRouter(VirtualGamepadBus& gamepads, VirtualKeyboard* keyboard)
    : gamepads_(gamepads), keyboard_(keyboard) {
}

void InputRouter::set_seat_assignment(SeatAssignment assignment) {
    std::lock_guard lock(mutex_);
    assignment_ = std::move(assignment);
    last_input_timestamp_by_player_.clear();
}

bool InputRouter::client_has_seat(ClientId client_id) const {
    for (const auto& seat : assignment_.seats) {
        if (seat.client_id == client_id) {
            return true;
        }
    }
    return false;
}

bool InputRouter::route(const ControllerInput& input) {
    std::lock_guard lock(mutex_);
    const auto port = find_retroarch_port(assignment_, input.client_id, input.local_player);
    if (!port.has_value()) {
        return false;
    }

    const PlayerKey key{input.client_id, input.local_player};
    const auto last_timestamp = last_input_timestamp_by_player_.find(key);
    // Strictly older timestamps are late/reordered UDP; equal timestamps are allowed so
    // clients can send duplicate datagrams for loss recovery.
    if (last_timestamp != last_input_timestamp_by_player_.end() &&
        input.state.timestamp_us < last_timestamp->second) {
        return false;
    }

    last_input_timestamp_by_player_[key] = input.state.timestamp_us;
    gamepads_.update(*port, input.state);
    if (!first_input_logged_) {
        first_input_logged_ = true;
        std::cout
            << "First controller input applied: client " << static_cast<int>(input.client_id)
            << " local P" << static_cast<int>(input.local_player) + 1
            << " -> RetroArch P" << static_cast<int>(*port) + 1
            << " buttons=0x" << std::hex << input.state.buttons << std::dec << '\n';
    }
    if (!first_nonzero_input_logged_ &&
        (input.state.buttons != 0 || input.state.left_trigger != 0 ||
         input.state.right_trigger != 0 || input.state.left_x != 0 || input.state.left_y != 0 ||
         input.state.right_x != 0 || input.state.right_y != 0)) {
        first_nonzero_input_logged_ = true;
        std::cout
            << "First non-zero controller state applied: client "
            << static_cast<int>(input.client_id)
            << " buttons=0x" << std::hex << input.state.buttons << std::dec
            << " triggers=" << input.state.left_trigger << "/" << input.state.right_trigger
            << '\n';
    }
    return true;
}

bool InputRouter::route(const KeyboardInput& input) {
    std::lock_guard lock(mutex_);
    if (keyboard_ == nullptr) {
        return false;
    }
    // Fast-forward / pause / menu are session-wide. Viewers have no pad seat, so do
    // not require client_has_seat() — that made remoted keyboard a no-op for Viewer
    // joins (and any client briefly without a seat map).

    const auto last_timestamp = last_keyboard_timestamp_by_client_.find(input.client_id);
    if (last_timestamp != last_keyboard_timestamp_by_client_.end() &&
        input.state.timestamp_us < last_timestamp->second) {
        return false;
    }
    last_keyboard_timestamp_by_client_[input.client_id] = input.state.timestamp_us;
    last_keyboard_client_ = input.client_id;
    keyboard_->apply(input.state);
    if (!first_keyboard_logged_) {
        first_keyboard_logged_ = true;
        std::cout
            << "First keyboard input applied: client " << static_cast<int>(input.client_id)
            << " keys=0x" << std::hex << input.state.keys << std::dec << '\n';
    }
    if (!first_nonzero_keyboard_logged_ && input.state.keys != 0) {
        first_nonzero_keyboard_logged_ = true;
        std::cout
            << "First non-zero keyboard keys from client "
            << static_cast<int>(input.client_id)
            << ": 0x" << std::hex << input.state.keys << std::dec
            << " (space=0x1 … f1=0x200 p=0x400)\n";
    }
    return true;
}

void InputRouter::apply_emulator_control(const EmulatorControl& control) {
    std::lock_guard lock(mutex_);
    if (keyboard_ == nullptr) {
        return;
    }
    if (!client_has_seat(control.client_id) && control.client_id != HostClientId) {
        return;
    }
    keyboard_->apply_emulator_control(control);
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

    if (keyboard_ != nullptr && last_keyboard_client_.has_value() &&
        *last_keyboard_client_ == client_id) {
        keyboard_->release_all();
        last_keyboard_client_.reset();
    }
}

bool InputRouter::PlayerKey::operator<(const PlayerKey& other) const {
    if (client_id != other.client_id) {
        return client_id < other.client_id;
    }

    return local_player < other.local_player;
}

} // namespace archstreamer
