#pragma once

#include "host/contracts/seat.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace archstreamer {

struct ProcessEnvironment;

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
