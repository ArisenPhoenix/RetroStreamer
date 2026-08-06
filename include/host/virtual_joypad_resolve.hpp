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

// Prefer PadPlan (include/host/pad_plan.hpp): resolve once, apply_pad_plan for env,
// binders read pads/indices. These helpers remain the low-level SDL/udev scanners.

// VID/PID whitelist helper (ArchStreamer uinput pads) in SDL hint form.
std::string sdl_archstreamer_pad_whitelist(
    std::size_t players,
    std::uint16_t product_id_base = 0);

/**
 * SDL_GAMECONTROLLER_IGNORE_DEVICES list that hides sibling-session ArchStreamer
 * pads (and optional physical controllers) while leaving this session's
 * [product_id_base, product_id_base+players) pads visible.
 *
 * Prefer this over IGNORE_DEVICES_EXCEPT for exclusive sessions: EXCEPT is
 * unreliable for ArchStreamer uinput pads under Ryujinx/gamescope, so a sibling
 * pad can stay at SDL index 0 and steal the bind mid-session.
 */
std::string sdl_archstreamer_sibling_ignore_list(
    std::size_t players,
    std::uint16_t product_id_base = 0,
    const std::string& physical_ignore = {});

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

struct ArchStreamerPadBinding {
    std::vector<ArchStreamerSdlPad> pads;
    // SDL_GAMECONTROLLER_IGNORE_DEVICES value the child must run with for
    // `pads[i].sdl_index` to be the index it sees (sibling + physical blacklist).
    // Empty when the filtered scan found nothing / fell back.
    std::string sdl_device_filter;
};

// Resolve this session's pads as seen by a child that hides sibling ArchStreamer
// pads (and physical controllers) via IGNORE. Emulators that key bindings on the
// SDL joystick index (Ryujinx) otherwise break when another session's uinput pad
// enumerates ahead of ours. Falls back to `fallback` with an empty filter when the
// filtered scan finds nothing.
ArchStreamerPadBinding resolve_exclusive_archstreamer_pads(
    std::size_t players,
    bool verbose,
    std::uint16_t product_id_base,
    std::vector<ArchStreamerSdlPad> fallback = {},
    const std::string& physical_ignore = {});

// RetroArch `udev` joypad slot indices for ArchStreamer uinput pads.
// Matches RetroArch vacant-slot order (joystick appearance in /proc), NOT kernel jsN.
std::vector<std::size_t> find_archstreamer_udev_joypad_indices(
    std::size_t players,
    bool verbose = false,
    std::uint16_t product_id_base = 0);

} // namespace archstreamer
