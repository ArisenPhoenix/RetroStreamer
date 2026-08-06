#include "host/virtual_joypad_resolve.hpp"

#include "common/protocol.hpp"
#include "host/host_session_helpers.hpp"
#include "host/platform/host_pad_platform.hpp"
#include "host/virtual_gamepad.hpp"

#include <SDL.h>

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace archstreamer {
namespace {

constexpr std::uint16_t kVirtualVendorId = 0x1209;
constexpr std::uint16_t kVirtualProductIdBase = 0xa517;

bool is_archstreamer_joystick(int device_index, std::uint16_t product_id_base, std::size_t players) {
    return platform_is_host_virtual_joystick(device_index, product_id_base, players);
}

std::string guid_string_for_index(int device_index) {
    char guid_text[33] = {};
    SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(device_index), guid_text, sizeof(guid_text));
    return guid_text;
}

// Yuzu's SDL driver zeroes the controller-name CRC (bytes 2-3) before storing/matching
// GUIDs. Bindings that keep the host SDL CRC never match at runtime.
std::string yuzu_filtered_sdl_guid(std::string guid) {
    if (guid.size() < 8) {
        return guid;
    }
    guid[4] = '0';
    guid[5] = '0';
    guid[6] = '0';
    guid[7] = '0';
    return guid;
}

// Must match the child's environment: filtered-out pads disappear from SDL's joystick
// list and renumber the remaining devices (often moving ArchStreamer from 2 → 0).
// Use process hints only for the scan — never setenv, or the host bridge can no longer
// open the real pad afterward.
struct SdlJoystickFilterHints {
    SdlJoystickFilterHints(const std::string& ignore_devices, const std::string& only_devices) {
        SDL_SetHint(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES, ignore_devices.c_str());
        SDL_SetHint(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT, only_devices.c_str());
    }
    ~SdlJoystickFilterHints() {
        SDL_SetHint(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES, "");
        SDL_SetHint(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT, "");
    }
    SdlJoystickFilterHints(const SdlJoystickFilterHints&) = delete;
    SdlJoystickFilterHints& operator=(const SdlJoystickFilterHints&) = delete;
};

std::vector<ArchStreamerSdlPad> scan_archstreamer_sdl_pads(
    std::size_t players,
    const std::string& ignore_devices,
    const std::string& only_devices,
    bool verbose,
    std::uint16_t product_id_base);

} // namespace

std::string sdl_archstreamer_pad_whitelist(std::size_t players, std::uint16_t product_id_base) {
    const auto base = product_id_base != 0 ? product_id_base : kVirtualProductIdBase;
    const auto count = std::max<std::size_t>(players, 1);
    std::string result;
    for (std::size_t port = 0; port < count && port < MaxRetroArchPorts; ++port) {
        if (!result.empty()) {
            result += ",";
        }
        result += hex_vid_pid(kVirtualVendorId, static_cast<std::uint16_t>(base + port));
    }
    return result;
}

std::string sdl_archstreamer_sibling_ignore_list(
    std::size_t players,
    std::uint16_t product_id_base,
    const std::string& physical_ignore) {
    // Concurrent slots use product_id_base = 0xa517 + slot*8 (see apply_slot_product_id_offset).
    // Reserve the full band so stale pads from older crashes are also hidden.
    constexpr int kSlotStride = 8;
    constexpr int kMaxSlots = 4; // matches clamp_max_session_slots upper bound
    const auto own_base = product_id_base != 0 ? product_id_base : kVirtualProductIdBase;
    const auto own_count = std::max<std::size_t>(players, 1);
    const auto own_end = static_cast<std::uint16_t>(own_base + own_count);

    std::string result = physical_ignore;
    const auto band_end =
        static_cast<std::uint16_t>(kVirtualProductIdBase + kSlotStride * kMaxSlots);
    for (std::uint16_t product = kVirtualProductIdBase; product < band_end; ++product) {
        if (product >= own_base && product < own_end) {
            continue;
        }
        if (!result.empty()) {
            result += ",";
        }
        result += hex_vid_pid(kVirtualVendorId, product);
    }
    return result;
}

std::vector<ArchStreamerSdlPad> find_archstreamer_sdl_pads(
    std::size_t players,
    const std::string& ignore_devices,
    bool verbose,
    std::uint16_t product_id_base) {
    return scan_archstreamer_sdl_pads(
        players, ignore_devices, /*only_devices=*/{}, verbose, product_id_base);
}

ArchStreamerPadBinding resolve_exclusive_archstreamer_pads(
    std::size_t players,
    bool verbose,
    std::uint16_t product_id_base,
    std::vector<ArchStreamerSdlPad> fallback,
    const std::string& physical_ignore) {
    auto ignore = sdl_archstreamer_sibling_ignore_list(players, product_id_base, physical_ignore);
    auto pads = scan_archstreamer_sdl_pads(
        players, ignore, /*only_devices=*/{}, verbose, product_id_base);
    if (pads.empty()) {
        return {std::move(fallback), {}};
    }
    return {std::move(pads), std::move(ignore)};
}

namespace {

std::vector<ArchStreamerSdlPad> scan_archstreamer_sdl_pads(
    std::size_t players,
    const std::string& ignore_devices,
    const std::string& only_devices,
    bool verbose,
    std::uint16_t product_id_base) {
    const SdlJoystickFilterHints hints{ignore_devices, only_devices};

    if (SDL_WasInit(SDL_INIT_JOYSTICK) != 0) {
        SDL_QuitSubSystem(SDL_INIT_JOYSTICK);
    }
    if (SDL_WasInit(SDL_INIT_GAMECONTROLLER) != 0) {
        SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER);
    }
    if (SDL_InitSubSystem(SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER) != 0) {
        std::cerr << "Warning: SDL joystick init failed while resolving virtual pads: "
                  << SDL_GetError() << '\n';
        return {};
    }

    SDL_JoystickUpdate();
    const int count = SDL_NumJoysticks();
    if (count < 0) {
        std::cerr << "Warning: SDL_NumJoysticks failed: " << SDL_GetError() << '\n';
        SDL_QuitSubSystem(SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER);
        return {};
    }

    if (verbose) {
        std::cout << "SDL joysticks after ignore filter: " << count << '\n';
    }
    std::vector<std::pair<std::uint16_t, ArchStreamerSdlPad>> found;
    for (int index = 0; index < count; ++index) {
        const char* name = SDL_JoystickNameForIndex(index);
        const auto vendor = static_cast<std::uint16_t>(SDL_JoystickGetDeviceVendor(index));
        const auto product = static_cast<std::uint16_t>(SDL_JoystickGetDeviceProduct(index));
        const bool virtual_pad = is_archstreamer_joystick(index, product_id_base, players);
        if (verbose) {
            std::cout
                << "  [" << index << "] " << (name != nullptr ? name : "?")
                << " " << hex_vid_pid(vendor, product)
                << (virtual_pad ? " (virtual)" : "") << '\n';
        }
        if (!virtual_pad) {
            continue;
        }
        found.push_back({
            product,
            ArchStreamerSdlPad{
                static_cast<std::size_t>(index),
                yuzu_filtered_sdl_guid(guid_string_for_index(index)),
                guid_string_for_index(index),
                product,
            },
        });
    }

    std::sort(found.begin(), found.end(), [](const auto& left, const auto& right) {
        return left.first < right.first;
    });

    std::vector<ArchStreamerSdlPad> pads;
    pads.reserve(players);
    for (std::size_t i = 0; i < players && i < found.size(); ++i) {
        pads.push_back(std::move(found[i].second));
    }

    SDL_QuitSubSystem(SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER);
    return pads;
}

} // namespace

std::vector<std::size_t> find_archstreamer_sdl_joypad_indices(
    std::size_t players,
    const std::string& ignore_devices,
    bool verbose,
    std::uint16_t product_id_base) {
    const auto pads = find_archstreamer_sdl_pads(players, ignore_devices, verbose, product_id_base);
    std::vector<std::size_t> indices;
    indices.reserve(pads.size());
    for (const auto& pad : pads) {
        indices.push_back(pad.sdl_index);
    }
    return indices;
}

std::vector<std::size_t> find_archstreamer_udev_joypad_indices(
    std::size_t players,
    bool verbose,
    std::uint16_t product_id_base) {
    // RetroArch's udev joypad driver does NOT use kernel jsN as the pad index.
    // It opens every ID_INPUT_JOYSTICK event node (udev_enumerate order ≈ /proc
    // joystick appearance order) and assigns vacant slots 0,1,2,… via
    // udev_find_vacant_pad(). input_playerN_joypad_index selects that slot.
    //
    // Concurrent sessions each plug a unique VID/PID uinput pad, but every
    // RetroArch still sees every sibling pad. If we bind using jsN (merk's pad
    // is often js0 while cloud is js1), a later slot can get joypad_index=0 and
    // actually read the earlier kid's pad — cross-session button bleed.
    std::ifstream in("/proc/bus/input/devices");
    if (!in) {
        std::cerr << "Warning: cannot read /proc/bus/input/devices for udev pad indices\n";
        return {};
    }

    const auto base = product_id_base != 0 ? product_id_base : kVirtualProductIdBase;
    const auto span = std::max<std::size_t>(players, 1);

    std::vector<std::pair<std::uint16_t, std::size_t>> found;
    std::string line;
    std::uint16_t vendor = 0;
    std::uint16_t product = 0;
    std::string name;
    std::size_t ordinal = 0;
    while (std::getline(in, line)) {
        if (line.rfind("I:", 0) == 0) {
            vendor = 0;
            product = 0;
            name.clear();
            const auto vpos = line.find("Vendor=");
            const auto ppos = line.find("Product=");
            if (vpos != std::string::npos) {
                vendor = static_cast<std::uint16_t>(std::stoul(line.substr(vpos + 7, 4), nullptr, 16));
            }
            if (ppos != std::string::npos) {
                product = static_cast<std::uint16_t>(std::stoul(line.substr(ppos + 8, 4), nullptr, 16));
            }
        } else if (line.rfind("N: Name=", 0) == 0) {
            name = line.substr(8);
            if (!name.empty() && name.front() == '"') {
                name.erase(0, 1);
            }
            if (!name.empty() && name.back() == '"') {
                name.pop_back();
            }
        } else if (line.rfind("H: Handlers=", 0) == 0) {
            const auto js_pos = line.find("js");
            if (js_pos == std::string::npos) {
                continue;
            }
            std::size_t js_number = 0;
            try {
                js_number = static_cast<std::size_t>(std::stoul(line.substr(js_pos + 2)));
            } catch (const std::exception&) {
                continue;
            }
            const std::size_t ra_index = ordinal++;
            const bool virtual_pad =
                (vendor == kVirtualVendorId &&
                 product >= base &&
                 product < static_cast<std::uint16_t>(base + span)) ||
                (product_id_base == 0 && name.rfind("ArchStreamer", 0) == 0);
            if (verbose) {
                std::cout
                    << "  udev-ra[" << ra_index << "] js" << js_number << " " << name << " "
                    << hex_vid_pid(vendor, product)
                    << (virtual_pad ? " (slot match)" : "") << '\n';
            }
            if (!virtual_pad) {
                continue;
            }
            found.push_back({product, ra_index});
        }
    }

    std::sort(found.begin(), found.end(), [](const auto& left, const auto& right) {
        return left.first < right.first;
    });

    std::vector<std::size_t> indices;
    indices.reserve(players);
    for (std::size_t i = 0; i < players && i < found.size(); ++i) {
        indices.push_back(found[i].second);
    }
    return indices;
}

} // namespace archstreamer
