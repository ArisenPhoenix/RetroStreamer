#include "common/controller_normalization.hpp"

namespace archstreamer {

std::int16_t normalize_axis(std::int16_t value, std::int16_t deadzone) {
    if (std::abs(static_cast<int>(value)) <= deadzone) {
        return 0;
    }

    return value;
}

std::uint16_t normalize_trigger(std::int16_t value, std::int16_t deadzone) {
    if (value <= deadzone) {
        return 0;
    }

    const auto clamped = std::clamp(static_cast<int>(value), 0, static_cast<int>(ControllerAxisMax));
    return static_cast<std::uint16_t>((static_cast<std::uint32_t>(clamped) * UINT16_MAX) / ControllerAxisMax);
}

} // namespace archstreamer
