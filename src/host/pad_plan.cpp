#include "host/pad_plan.hpp"

#include "host/launch_environment.hpp"

#include <iostream>
#include <utility>

namespace archstreamer {

PadPlan resolve_shared_pad_plan(
    std::size_t players,
    const std::string& ignore_devices,
    bool verbose,
    std::uint16_t product_id_base,
    bool use_udev) {
    PadPlan plan;
    plan.mode = PadPlanMode::Shared;
    plan.ignore_devices = ignore_devices;
    if (use_udev) {
        plan.udev_indices =
            find_archstreamer_udev_joypad_indices(players, verbose, product_id_base);
        for (std::size_t i = 0; i < plan.udev_indices.size(); ++i) {
            ArchStreamerSdlPad pad;
            pad.sdl_index = plan.udev_indices[i];
            pad.product_id = static_cast<std::uint16_t>(
                (product_id_base != 0 ? product_id_base : 0xa517) + i);
            plan.pads.push_back(std::move(pad));
        }
    } else {
        plan.pads =
            find_archstreamer_sdl_pads(players, ignore_devices, verbose, product_id_base);
    }
    return plan;
}

PadPlan resolve_exclusive_pad_plan(
    std::size_t players,
    bool verbose,
    std::uint16_t product_id_base,
    std::vector<ArchStreamerSdlPad> fallback,
    const std::string& physical_ignore) {
    const auto binding = resolve_exclusive_archstreamer_pads(
        players, verbose, product_id_base, std::move(fallback), physical_ignore);

    PadPlan plan;
    if (binding.sdl_device_filter.empty()) {
        // Filtered scan failed — degrade to shared physical IGNORE only.
        plan.mode = PadPlanMode::Shared;
        plan.pads = binding.pads;
        plan.ignore_devices = physical_ignore;
        return plan;
    }

    plan.mode = PadPlanMode::Exclusive;
    plan.pads = std::move(binding.pads);
    plan.ignore_devices = binding.sdl_device_filter;
    plan.exclusive_filter = sdl_archstreamer_pad_whitelist(players, product_id_base);
    // Child under sibling IGNORE enumerates only these pads as 0..n-1 regardless of
    // host kernel jsN order. Bindings must use child-facing indices.
    for (std::size_t i = 0; i < plan.pads.size(); ++i) {
        plan.pads[i].sdl_index = i;
    }
    return plan;
}

PadPlan resolve_retroarch_slot_pad_plan(
    std::size_t players,
    const std::string& ignore_devices,
    bool verbose,
    std::uint16_t product_id_base,
    bool use_udev) {
    // Start from the absolute-index scan RetroArch udev needs.
    auto plan = resolve_shared_pad_plan(
        players, ignore_devices, verbose, product_id_base, use_udev);

    const auto keep = sdl_archstreamer_pad_whitelist(players, product_id_base);
    const auto ignore =
        sdl_archstreamer_sibling_ignore_list(players, product_id_base, ignore_devices);
    if (ignore.empty() || plan.pads.empty()) {
        return plan;
    }

    // Hide sibling-session ArchStreamer pads (and physical pads) from SDL via IGNORE.
    // RetroArch's udev driver still opens every ID_INPUT_JOYSTICK event node and numbers
    // them as vacant slots 0..n-1 in discovery order (not kernel jsN). joypad_index must
    // be that discovery ordinal for this slot's VID/PID — do not remap to 0..n-1 the way
    // Ryujinx exclusive plans do.
    plan.mode = PadPlanMode::Exclusive;
    plan.exclusive_filter = keep;
    plan.ignore_devices = ignore;
    return plan;
}

void apply_pad_plan(ProcessEnvironment& env, const PadPlan& plan) {
    // Always clear EXCEPT. Exclusive used to rely on IGNORE_DEVICES_EXCEPT alone; that
    // fails to hide ArchStreamer uinput siblings under Ryujinx, leaving the other kid's
    // pad at SDL index 0 and stealing the bind mid-session.
    env.clear_var("SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT");
    if (!plan.ignore_devices.empty()) {
        env.set("SDL_GAMECONTROLLER_IGNORE_DEVICES", plan.ignore_devices);
    } else {
        env.clear_var("SDL_GAMECONTROLLER_IGNORE_DEVICES");
    }
}

void log_pad_plan(const PadPlan& plan, std::optional<int> slot_index) {
    std::ostream& out = std::cout;
    if (slot_index.has_value()) {
        out << "session slot " << *slot_index << ": ";
    }
    out << "PadPlan mode=" << (plan.exclusive() ? "exclusive" : "shared")
        << " pads=" << plan.pads.size();
    if (plan.exclusive()) {
        out << " keep=" << (plan.exclusive_filter.empty() ? "(none)" : plan.exclusive_filter);
        out << " ignore=" << (plan.ignore_devices.empty() ? "(none)" : plan.ignore_devices);
        if (!plan.udev_indices.empty()) {
            out << " udev-abs";
        }
    } else {
        out << " ignore=" << (plan.ignore_devices.empty() ? "(none)" : plan.ignore_devices);
        if (!plan.udev_indices.empty()) {
            out << " udev";
        }
    }
    for (std::size_t i = 0; i < plan.pads.size(); ++i) {
        out << " [" << i << "]=sdl:" << plan.pads[i].sdl_index;
        if (!plan.pads[i].guid.empty()) {
            out << " guid:" << plan.pads[i].guid.substr(0, 8) << "…";
        }
    }
    out << '\n';
}

} // namespace archstreamer
