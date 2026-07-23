#include "host/virtual_joypad_resolve.hpp"

#include "common/protocol.hpp"
#include "host/host_session_helpers.hpp"
#include "host/linux_uinput_gamepad.hpp"

#include <SDL.h>

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace archstreamer {
namespace {

constexpr std::uint16_t kVirtualVendorId = 0x1209;
constexpr std::uint16_t kVirtualProductIdBase = 0xa517;

bool is_archstreamer_joystick(int device_index) {
    const auto vendor = static_cast<std::uint16_t>(SDL_JoystickGetDeviceVendor(device_index));
    const auto product = static_cast<std::uint16_t>(SDL_JoystickGetDeviceProduct(device_index));
    if (vendor == kVirtualVendorId &&
        product >= kVirtualProductIdBase &&
        product < kVirtualProductIdBase + MaxRetroArchPorts) {
        return true;
    }

    const char* name = SDL_JoystickNameForIndex(device_index);
    return name != nullptr && std::string(name).rfind("ArchStreamer", 0) == 0;
}

} // namespace

std::string sdl_archstreamer_pad_whitelist(std::size_t players) {
    const auto count = std::max<std::size_t>(players, 1);
    std::string result;
    for (std::size_t port = 0; port < count && port < MaxRetroArchPorts; ++port) {
        if (!result.empty()) {
            result += ",";
        }
        result += hex_vid_pid(
            kVirtualVendorId,
            static_cast<std::uint16_t>(kVirtualProductIdBase + port));
    }
    return result;
}

std::vector<std::size_t> find_archstreamer_sdl_joypad_indices(
    std::size_t players,
    const std::string& ignore_devices) {
    // Must match RetroArch's environment: ignored pads disappear from SDL's joystick
    // list and renumber remaining devices (often moving ArchStreamer from 2 → 0).
    // Use a process hint only for this scan — never setenv, or the host bridge can no
    // longer open the real pad afterward.
    if (!ignore_devices.empty()) {
        SDL_SetHint(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES, ignore_devices.c_str());
    } else {
        SDL_SetHint(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES, "");
    }

    if (SDL_WasInit(SDL_INIT_JOYSTICK) != 0) {
        SDL_QuitSubSystem(SDL_INIT_JOYSTICK);
    }
    if (SDL_WasInit(SDL_INIT_GAMECONTROLLER) != 0) {
        SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER);
    }
    if (SDL_InitSubSystem(SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER) != 0) {
        std::cerr << "Warning: SDL joystick init failed while resolving virtual pads: "
                  << SDL_GetError() << '\n';
        SDL_SetHint(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES, "");
        return {};
    }

    SDL_JoystickUpdate();
    const int count = SDL_NumJoysticks();
    if (count < 0) {
        std::cerr << "Warning: SDL_NumJoysticks failed: " << SDL_GetError() << '\n';
        SDL_QuitSubSystem(SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER);
        SDL_SetHint(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES, "");
        return {};
    }

    std::cout << "SDL joysticks after ignore filter: " << count << '\n';
    std::vector<std::pair<std::uint16_t, int>> found;
    for (int index = 0; index < count; ++index) {
        const char* name = SDL_JoystickNameForIndex(index);
        const auto vendor = static_cast<std::uint16_t>(SDL_JoystickGetDeviceVendor(index));
        const auto product = static_cast<std::uint16_t>(SDL_JoystickGetDeviceProduct(index));
        std::cout
            << "  [" << index << "] " << (name != nullptr ? name : "?")
            << " " << hex_vid_pid(vendor, product)
            << (is_archstreamer_joystick(index) ? " (virtual)" : "") << '\n';
        if (!is_archstreamer_joystick(index)) {
            continue;
        }
        found.push_back({product, index});
    }

    std::sort(found.begin(), found.end(), [](const auto& left, const auto& right) {
        return left.first < right.first;
    });

    std::vector<std::size_t> indices;
    indices.reserve(players);
    for (std::size_t i = 0; i < players && i < found.size(); ++i) {
        indices.push_back(static_cast<std::size_t>(found[i].second));
    }

    SDL_QuitSubSystem(SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER);
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES, "");
    return indices;
}

} // namespace archstreamer
