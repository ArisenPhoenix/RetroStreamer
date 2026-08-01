#pragma once

#include <cstdint>

struct ControllerState {
    std::uint32_t sequence = 0;
    std::uint64_t timestamp_us = 0;
    std::uint32_t buttons = 0;

    std::int16_t left_x = 0;
    std::int16_t left_y = 0;
    std::int16_t right_x = 0;
    std::int16_t right_y = 0;

    std::uint16_t left_trigger = 0;
    std::uint16_t right_trigger = 0;
};

enum ControllerButton : std::uint32_t {
    ButtonA = 1u << 0, // South (Cross / Xbox A)
    ButtonB = 1u << 1, // East  (Circle / Xbox B)
    ButtonX = 1u << 2, // West  (Square / Xbox X)
    ButtonY = 1u << 3, // North (Triangle / Xbox Y)
    ButtonBack = 1u << 4,
    ButtonGuide = 1u << 5,
    ButtonStart = 1u << 6,
    ButtonLeftStick = 1u << 7,
    ButtonRightStick = 1u << 8,
    ButtonLeftShoulder = 1u << 9,
    ButtonRightShoulder = 1u << 10,
    ButtonDpadUp = 1u << 11,
    ButtonDpadDown = 1u << 12,
    ButtonDpadLeft = 1u << 13,
    ButtonDpadRight = 1u << 14,
};

/** Swap face bits by NESW position before they hit the wire. Host stays a dumb relay. */
inline void apply_face_button_swaps(ControllerState& state, bool swap_nw, bool swap_se) {
    if (!swap_nw && !swap_se) {
        return;
    }
    auto buttons = state.buttons;
    if (swap_se) {
        const bool south = (buttons & ButtonA) != 0;
        const bool east = (buttons & ButtonB) != 0;
        buttons = (buttons & ~(ButtonA | ButtonB))
            | (east ? ButtonA : 0)
            | (south ? ButtonB : 0);
    }
    if (swap_nw) {
        const bool west = (buttons & ButtonX) != 0;
        const bool north = (buttons & ButtonY) != 0;
        buttons = (buttons & ~(ButtonX | ButtonY))
            | (north ? ButtonX : 0)
            | (west ? ButtonY : 0);
    }
    state.buttons = buttons;
}
