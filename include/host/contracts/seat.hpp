#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace archstreamer {

/**
 * One ArchStreamer virtual pad as the emulator child will see it.
 * Peripherals resolve this after plug; emulator binders only read it.
 */
struct ArchStreamerSdlPad {
    std::size_t sdl_index = 0;
    // CRC bytes zeroed — Yuzu/Ryujinx store/match GUIDs this way.
    std::string guid;
    // Full SDL GUID (with name CRC) for SDL_GAMECONTROLLERCONFIG matching.
    std::string mapping_guid;
    std::uint16_t product_id = 0;
};

/** Exclusive-mode bind: pads plus the IGNORE list the child must inherit. */
struct ArchStreamerPadBinding {
    std::vector<ArchStreamerSdlPad> pads;
    // SDL_GAMECONTROLLER_IGNORE_DEVICES value the child must run with for
    // `pads[i].sdl_index` to be the index it sees (sibling + physical blacklist).
    // Empty when the filtered scan found nothing / fell back.
    std::string sdl_device_filter;
};

/**
 * Per-session seat language: peripherals produce it, emulators consume it.
 *
 * Resolve once after uinput/ViGEm plug; emulator binders and launch env only
 * read this. Two modes:
 *   Shared    — RetroArch (legacy single-session) / Yuzu: IGNORE physical pads;
 *               keep host SDL or udev indices
 *   Exclusive — Ryujinx / melonDS / concurrent RetroArch: IGNORE sibling
 *               ArchStreamer pads (+ physical); child sees only this session
 *
 * Concurrent RetroArch keeps absolute host udev discovery ordinals in
 * udev_indices (unlike Ryujinx, which remaps child indices to 0..n-1 once
 * siblings are hidden).
 *
 * Never set SDL_GAMECONTROLLER_IGNORE_DEVICES to "" — use clear_var / unset.
 * Prefer IGNORE blacklists over EXCEPT whitelists for exclusive sessions:
 * EXCEPT does not reliably hide ArchStreamer uinput pads under Ryujinx.
 */
enum class PadPlanMode : std::uint8_t {
    Shared = 0,
    Exclusive = 1,
};

struct PadPlan {
    PadPlanMode mode = PadPlanMode::Shared;
    std::vector<ArchStreamerSdlPad> pads;
    /** Own-pad VID/PID list (logging / diagnostics; not applied as EXCEPT). */
    std::string exclusive_filter;
    /**
     * SDL_GAMECONTROLLER_IGNORE_DEVICES.
     * Shared: physical blacklist. Exclusive: sibling ArchStreamer + physical.
     */
    std::string ignore_devices;
    /** When Shared/Exclusive + udev joypad driver — absolute host jsN indices. */
    std::vector<std::size_t> udev_indices;

    [[nodiscard]] bool exclusive() const { return mode == PadPlanMode::Exclusive; }
};

} // namespace archstreamer
