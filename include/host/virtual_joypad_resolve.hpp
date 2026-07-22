#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace archstreamer {

// VID/PID whitelist helper (ArchStreamer uinput pads).
std::string sdl_archstreamer_pad_whitelist(std::size_t players);

// After plugging virtual pads, find their SDL joystick indices as RetroArch will see them.
// Pass the same SDL_GAMECONTROLLER_IGNORE_DEVICES list that will be set in RetroArch's env.
std::vector<std::size_t> find_archstreamer_sdl_joypad_indices(
    std::size_t players,
    const std::string& ignore_devices = {});

} // namespace archstreamer
