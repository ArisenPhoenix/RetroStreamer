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
    std::vector<ArchStreamerSdlPad> fallback) {
    const auto binding = resolve_exclusive_archstreamer_pads(
        players, verbose, product_id_base, std::move(fallback));

    PadPlan plan;
    plan.pads = std::move(binding.pads);
    if (binding.sdl_device_filter.empty()) {
        // Filtered scan failed — degrade to shared indices; caller should set
        // ignore_devices from the host blacklist before apply_pad_plan.
        plan.mode = PadPlanMode::Shared;
        return plan;
    }

    plan.mode = PadPlanMode::Exclusive;
    plan.exclusive_filter = binding.sdl_device_filter;
    // Child under EXCEPT enumerates only these pads as 0..n-1 regardless of host
    // kernel jsN order. Bindings must use child-facing indices.
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

    const auto filter = sdl_archstreamer_pad_whitelist(players, product_id_base);
    if (filter.empty() || plan.pads.empty()) {
        return plan;
    }

    // Hide sibling-session ArchStreamer pads (and physical pads) from SDL. RetroArch's
    // udev driver still opens every ID_INPUT_JOYSTICK event node and numbers them as
    // vacant slots 0..n-1 in discovery order (not kernel jsN). joypad_index must be
    // that discovery ordinal for this slot's VID/PID — do not remap to 0..n-1 the way
    // Ryujinx exclusive plans do.
    plan.mode = PadPlanMode::Exclusive;
    plan.exclusive_filter = filter;
    plan.ignore_devices.clear();
    return plan;
}

void apply_pad_plan(ProcessEnvironment& env, const PadPlan& plan) {
    if (plan.exclusive()) {
        env.clear_var("SDL_GAMECONTROLLER_IGNORE_DEVICES");
        if (!plan.exclusive_filter.empty()) {
            env.set("SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT", plan.exclusive_filter);
        } else {
            env.clear_var("SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT");
        }
        return;
    }

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
        out << " filter=" << (plan.exclusive_filter.empty() ? "(none)" : plan.exclusive_filter);
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
