#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace archstreamer {

struct ArchStreamerSdlPad {
    std::size_t sdl_index = 0;
    // CRC bytes zeroed — Yuzu/Ryujinx store/match GUIDs this way.
    std::string guid;
    // Full SDL GUID (with name CRC) for SDL_GAMECONTROLLERCONFIG matching.
    std::string mapping_guid;
    std::uint16_t product_id = 0;
};

// VID/PID whitelist helper (ArchStreamer uinput pads).
std::string sdl_archstreamer_pad_whitelist(std::size_t players);

// After plugging virtual pads, find their SDL joystick indices as RetroArch will see them.
// Pass the same SDL_GAMECONTROLLER_IGNORE_DEVICES list that will be set in RetroArch's env.
// product_id_base: when non-zero, only match pads with product in [base, base+players).
std::vector<std::size_t> find_archstreamer_sdl_joypad_indices(
    std::size_t players,
    const std::string& ignore_devices = {},
    bool verbose = false,
    std::uint16_t product_id_base = 0);

// Same scan as above, but also returns SDL GUID strings (for Yuzu Controls bindings).
std::vector<ArchStreamerSdlPad> find_archstreamer_sdl_pads(
    std::size_t players,
    const std::string& ignore_devices = {},
    bool verbose = false,
    std::uint16_t product_id_base = 0);

// RetroArch `udev` joypad indices for ArchStreamer uinput pads (from /proc/bus/input/devices).
// Prefer this over SDL indices when input_joypad_driver=udev — the two enumerations differ.
std::vector<std::size_t> find_archstreamer_udev_joypad_indices(
    std::size_t players,
    bool verbose = false,
    std::uint16_t product_id_base = 0);

} // namespace archstreamer
