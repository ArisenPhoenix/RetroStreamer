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
 *   Shared    — RetroArch / Yuzu: IGNORE physical pads; keep host SDL or udev indices
 *   Exclusive — Ryujinx / melonDS: EXCEPT whitelist only; child indices are 0..n-1
 *
 * Never set SDL_GAMECONTROLLER_IGNORE_DEVICES to "" — use clear_var / unset.
 */
enum class PadPlanMode : std::uint8_t {
    Shared = 0,
    Exclusive = 1,
};

struct PadPlan {
    PadPlanMode mode = PadPlanMode::Shared;
    std::vector<ArchStreamerSdlPad> pads;
    /** SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT (Exclusive only). */
    std::string exclusive_filter;
    /** SDL_GAMECONTROLLER_IGNORE_DEVICES (Shared only; empty for Exclusive). */
    std::string ignore_devices;
    /** When Shared + udev joypad driver. */
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
 * Exclusive mode: scan under EXCEPT whitelist; pads[i].sdl_index is the index the
 * child will see (0..n-1). Falls back to unfiltered pads with empty filter.
 */
PadPlan resolve_exclusive_pad_plan(
    std::size_t players,
    bool verbose,
    std::uint16_t product_id_base,
    std::vector<ArchStreamerSdlPad> fallback = {});

/** Apply IGNORE / EXCEPT. Exclusive clears IGNORE entirely (never sets ""). */
void apply_pad_plan(ProcessEnvironment& env, const PadPlan& plan);

void log_pad_plan(const PadPlan& plan, std::optional<int> slot_index = std::nullopt);

} // namespace archstreamer
