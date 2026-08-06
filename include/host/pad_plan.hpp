#pragma once

#include "host/virtual_joypad_resolve.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace archstreamer {

struct ProcessEnvironment;

/**
 * Single per-session contract for ArchStreamer virtual pads.
 *
 * Resolve once after uinput plug; emulator binders and launch env only read this.
 * Two modes:
 *   Shared    — RetroArch (legacy single-session) / Yuzu: IGNORE physical pads;
 *               keep host SDL or udev indices
 *   Exclusive — Ryujinx / melonDS / concurrent RetroArch: IGNORE sibling
 *               ArchStreamer pads (+ physical); child sees only this session
 *
 * Concurrent RetroArch keeps absolute host udev discovery ordinals in
 * pads/udev_indices (unlike Ryujinx, which remaps child indices to 0..n-1 once
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

    bool exclusive() const { return mode == PadPlanMode::Exclusive; }
};

/** Shared mode: SDL or udev indices under the host IGNORE blacklist. */
PadPlan resolve_shared_pad_plan(
    std::size_t players,
    const std::string& ignore_devices,
    bool verbose,
    std::uint16_t product_id_base,
    bool use_udev);

/**
 * Exclusive mode: scan under sibling IGNORE blacklist; pads[i].sdl_index is the
 * index the child will see (0..n-1). Falls back to Shared with physical_ignore
 * when the filtered scan finds nothing.
 */
PadPlan resolve_exclusive_pad_plan(
    std::size_t players,
    bool verbose,
    std::uint16_t product_id_base,
    std::vector<ArchStreamerSdlPad> fallback = {},
    const std::string& physical_ignore = {});

/**
 * Concurrent RetroArch slots: IGNORE sibling ArchStreamer pads so they disappear
 * from SDL, but keep absolute udev discovery ordinals for
 * input_player*_joypad_index (udev driver ignores SDL filters).
 */
PadPlan resolve_retroarch_slot_pad_plan(
    std::size_t players,
    const std::string& ignore_devices,
    bool verbose,
    std::uint16_t product_id_base,
    bool use_udev);

/** Apply IGNORE (and clear EXCEPT). Never sets IGNORE to "". */
void apply_pad_plan(ProcessEnvironment& env, const PadPlan& plan);

void log_pad_plan(const PadPlan& plan, std::optional<int> slot_index = std::nullopt);

} // namespace archstreamer
