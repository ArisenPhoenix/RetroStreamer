#include "host/platform/host_pad_platform.hpp"

#include "host/virtual_gamepad.hpp"

#include <SDL.h>

#include <algorithm>
#include <string>

namespace archstreamer {
namespace {

constexpr std::uint16_t kVirtualVendorId = 0x1209;
constexpr std::uint16_t kVirtualProductIdBase = 0xa517;

} // namespace

bool platform_is_host_virtual_joystick(
    int device_index,
    std::uint16_t product_id_base,
    std::size_t players) {
    const auto vendor = static_cast<std::uint16_t>(SDL_JoystickGetDeviceVendor(device_index));
    const auto product = static_cast<std::uint16_t>(SDL_JoystickGetDeviceProduct(device_index));
    const auto base = product_id_base != 0 ? product_id_base : kVirtualProductIdBase;
    const auto span = std::max<std::size_t>(players, 1);

    // Preferred: ViGEm targets stamped with ArchStreamer VID/PID (see ViGEmGamepadBus).
    if (vendor == kVirtualVendorId &&
        product >= base &&
        product < static_cast<std::uint16_t>(base + span)) {
        return true;
    }

    const char* name = SDL_JoystickNameForIndex(device_index);
    if (name != nullptr && std::string(name).rfind("ArchStreamer", 0) == 0) {
        return true;
    }

#if defined(_WIN32)
    // Fallback when the bus could not stamp custom VID/PID: ViGEm X360 pads often
    // enumerate as generic Xbox 360 Controller. Match only when the caller did not
    // request a specific product-id span (session product_id_base == 0).
    if (product_id_base == 0 && name != nullptr) {
        const std::string n(name);
        if (n.find("Xbox 360") != std::string::npos ||
            n.find("XBOX 360") != std::string::npos ||
            n.find("XInput") != std::string::npos) {
            return true;
        }
    }
#endif
    return false;
}

} // namespace archstreamer
