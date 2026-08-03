#pragma once

#include "common/protocol.hpp"
#include "host/seat_manager.hpp"
#include "host/virtual_gamepad.hpp"
#include "host/virtual_keyboard.hpp"

#include <functional>
#include <map>
#include <mutex>
#include <optional>

namespace archstreamer {

class InputRouter {
public:
    using TouchHandler = std::function<bool(const TouchInput&)>;

    InputRouter(VirtualGamepadBus& gamepads, VirtualKeyboard* keyboard = nullptr);

    void set_seat_assignment(SeatAssignment assignment);
    /** Optional melonDS (etc.) stylus sink; cleared automatically when null. */
    void set_touch_handler(TouchHandler handler);
    bool route(const ControllerInput& input);
    bool route(const KeyboardInput& input);
    bool route(const TouchInput& input);
    void apply_emulator_control(const EmulatorControl& control);
    void neutralize_client(ClientId client_id);

private:
    struct PlayerKey {
        ClientId client_id = 0;
        LocalPlayerIndex local_player = 0;

        bool operator<(const PlayerKey& other) const;
    };

    bool client_has_seat(ClientId client_id) const;

    VirtualGamepadBus& gamepads_;
    VirtualKeyboard* keyboard_ = nullptr;
    TouchHandler touch_handler_;
    SeatAssignment assignment_;
    std::map<PlayerKey, std::uint64_t> last_input_timestamp_by_player_;
    std::map<ClientId, std::uint64_t> last_keyboard_timestamp_by_client_;
    std::map<PlayerKey, std::uint64_t> last_touch_timestamp_by_player_;
    std::optional<ClientId> last_keyboard_client_;
    bool first_input_logged_ = false;
    bool first_nonzero_input_logged_ = false;
    bool first_keyboard_logged_ = false;
    bool first_nonzero_keyboard_logged_ = false;
    bool first_touch_logged_ = false;
    mutable std::mutex mutex_;
};

} // namespace archstreamer
