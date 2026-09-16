#include "host/virtual/device_plan.hpp"

#include "client/controller_backend.hpp"
#include "host/virtual/virtual_gamepad.hpp"

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace archstreamer {
namespace {

constexpr const char* kSteamInputIgnoreDevices =
    "0x28de/0x11ff,0x28de/0x1205,0x28de/0x1201";

void append_ignore_devices(PadSettings& pads, const std::string& devices) {
    if (devices.empty()) {
        return;
    }
    if (pads.ignore_controller.has_value() && !pads.ignore_controller->empty()) {
        pads.ignore_controller = *pads.ignore_controller + "," + devices;
    } else {
        pads.ignore_controller = devices;
    }
}

} // namespace

SessionDevicePlan resolve_session_device_plan(
    const PadSettings& pads,
    bool verbose,
    std::uint8_t players,
    SessionPadPlanKind kind,
    std::uint16_t product_id_base) {
    SessionDevicePlan devices;
    devices.input.product_id_base = product_id_base;
    const bool use_udev = pads.retroarch_joypad_driver == "udev";
    if (verbose && use_udev && kind == SessionPadPlanKind::Direct) {
        std::cout << "udev joysticks (ArchStreamer hunt):\n";
    }
    if (kind == SessionPadPlanKind::RetroArchSlot) {
        devices.input.shared_pad_plan = resolve_retroarch_slot_pad_plan(
            players,
            pads.ignore_controller.value_or(""),
            verbose,
            product_id_base,
            use_udev);
    } else {
        devices.input.shared_pad_plan = resolve_shared_pad_plan(
            players,
            pads.ignore_controller.value_or(""),
            verbose,
            product_id_base,
            use_udev);
    }

    if (use_udev) {
        devices.input.resolved_indices = devices.input.shared_pad_plan.udev_indices;
        devices.input.resolved_pads = devices.input.shared_pad_plan.pads;
    } else {
        devices.input.resolved_pads = devices.input.shared_pad_plan.pads;
        devices.input.resolved_indices.reserve(devices.input.resolved_pads.size());
        for (const auto& pad : devices.input.resolved_pads) {
            devices.input.resolved_indices.push_back(pad.sdl_index);
        }
    }
    if (pads.virtual_joypad_index.has_value()) {
        devices.input.virtual_joypad_index = *pads.virtual_joypad_index;
        if (kind == SessionPadPlanKind::Direct) {
            std::cout
                << "Using explicit --virtual-joypad-index "
                << devices.input.virtual_joypad_index << '\n';
        }
    } else if (!devices.input.resolved_indices.empty()) {
        devices.input.virtual_joypad_index = devices.input.resolved_indices.front();
        if (kind == SessionPadPlanKind::Direct && verbose) {
            std::cout
                << "Resolved virtual joypad index " << devices.input.virtual_joypad_index
                << " (driver=" << pads.retroarch_joypad_driver << ")\n";
        }
    } else if (kind == SessionPadPlanKind::Direct) {
        std::cerr
            << "Warning: ArchStreamer virtual pads not visible to "
            << pads.retroarch_joypad_driver
            << " yet; defaulting RetroArch joypad index to 0.\n";
    }
    return devices;
}

void plug_session_gamepads(VirtualGamepadBus& gamepads, RetroArchPort players) {
    for (RetroArchPort port = 0; port < players; ++port) {
        gamepads.plug(port);
    }
}

void wait_for_session_input_enumeration() {
    std::this_thread::sleep_for(std::chrono::milliseconds(750));
}

void append_session_controller_ignore_list(
    PadSettings& pads,
    const std::optional<ControllerDevice>& bridge_device,
    std::string_view log_prefix,
    bool warn_if_not_sdl2) {
    if (bridge_device.has_value() && !pads.ignore_controller.has_value()) {
        if (bridge_device->vendor_id != 0 && bridge_device->product_id != 0) {
            pads.ignore_controller = hex_vid_pid(
                bridge_device->vendor_id,
                bridge_device->product_id);
        }
    }

    try {
        ControllerBackend host_pads;
        std::string host_ignore;
        for (const auto& device : host_pads.list_devices()) {
            if (device.vendor_id == 0 || device.product_id == 0) {
                continue;
            }
            const auto id = hex_vid_pid(device.vendor_id, device.product_id);
            if (!host_ignore.empty()) {
                host_ignore += ",";
            }
            host_ignore += id;
        }
        append_ignore_devices(pads, host_ignore);
    } catch (const std::exception& error) {
        if (log_prefix.empty()) {
            std::cerr
                << "Warning: host controller scan for ignore list failed: "
                << error.what() << '\n';
        } else {
            std::cerr
                << log_prefix
                << "warning: host controller scan failed: "
                << error.what() << '\n';
        }
    }

    append_ignore_devices(pads, kSteamInputIgnoreDevices);

    if (warn_if_not_sdl2 && pads.retroarch_joypad_driver != "sdl2") {
        std::cerr
            << "Warning: SDL_GAMECONTROLLER_IGNORE_DEVICES only affects RetroArch when "
            << "--retroarch-joypad-driver is sdl2.\n";
    }
}

} // namespace archstreamer
