#pragma once

#include <cstdint>

struct ControllerState {
    std::uint32_t sequence = 0;
    std::uint32_t buttons = 0;

    std::int16_t left_x = 0;
    std::int16_t left_y = 0;
    std::int16_t right_x = 0;
    std::int16_t right_y = 0;

    std::uint16_t left_trigger = 0;
    std::uint16_t right_trigger = 0;
};

enum ControllerButton : std::uint32_t {
    ButtonA = 1u << 0,
    ButtonB = 1u << 1,
    ButtonX = 1u << 2,
    ButtonY = 1u << 3,
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
