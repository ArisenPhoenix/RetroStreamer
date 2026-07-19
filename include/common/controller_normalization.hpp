#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>

namespace archstreamer {

constexpr std::int16_t DefaultStickDeadzone = 8000;
constexpr std::int16_t DefaultTriggerDeadzone = 768;
constexpr std::int16_t ControllerAxisMax = 32767;

std::int16_t normalize_axis(std::int16_t value, std::int16_t deadzone = DefaultStickDeadzone);
std::uint16_t normalize_trigger(std::int16_t value, std::int16_t deadzone = DefaultTriggerDeadzone);

} // namespace archstreamer
