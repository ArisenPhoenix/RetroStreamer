#include "host/host_session_helpers.hpp"
#include "host/local_controller_bridge.hpp"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace archstreamer {
namespace {

std::string resolve_live_device_id(const ControllerDevice& wanted) {
    ControllerBackend backend;
    const auto devices = backend.list_devices();
    for (const auto& device : devices) {
        if (!wanted.guid.empty() && device.guid == wanted.guid) {
            return device.id;
        }
    }
    for (const auto& device : devices) {
        if (wanted.vendor_id != 0 && wanted.product_id != 0 &&
            device.vendor_id == wanted.vendor_id &&
            device.product_id == wanted.product_id) {
            return device.id;
        }
    }
    for (const auto& device : devices) {
        if (!wanted.path.empty() && device.path == wanted.path) {
            return device.id;
        }
    }
    throw std::runtime_error(
        "local bridge controller is no longer visible to SDL (unplugged or ignored): " +
        wanted.name);
}

} // namespace

void pulse_virtual_pad_a(VirtualGamepadBus& gamepads) {
    ControllerState state;
    state.timestamp_us = 1;
    state.buttons = ButtonA;
    gamepads.update(0, state);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    state.timestamp_us = 2;
    state.buttons = 0;
    gamepads.update(0, state);
}

ControllerDevice local_bridge_device_for(std::size_t selected_index) {
    ControllerBackend backend;
    const auto devices = backend.list_devices();
    if (devices.empty()) {
        throw std::runtime_error("no SDL2 game controllers detected for local bridge");
    }
    if (selected_index >= devices.size()) {
        throw std::runtime_error("local bridge controller index is out of range");
    }

    return devices[selected_index];
}

LocalControllerBridge::LocalControllerBridge(ControllerDevice device)
    : device_(std::move(device)) {
    // RetroArch gets IGNORE via its own env; the host bridge must still see the real pad.
    unsetenv("SDL_GAMECONTROLLER_IGNORE_DEVICES");
    // Re-resolve by GUID/vid/pid. The SDL index captured at selection time is stale once
    // virtual ArchStreamer pads appear (and must not run under the RetroArch ignore list).
    const auto live_id = resolve_live_device_id(device_);
    std::cout
        << "Bridging local controller "
        << ": " << device_.name
        << " [selected id=" << device_.id << " live id=" << live_id << "]"
        << " vid/pid=" << hex_vid_pid(device_.vendor_id, device_.product_id)
        << "\n";

    backend_.open_selected({live_id});
}

const ControllerDevice& LocalControllerBridge::device() const {
    return device_;
}

void LocalControllerBridge::update(InputRouter& input_router) {
    const auto state = backend_.poll(0);
    if (state.has_value()) {
        input_router.route(ControllerInput{
            HostClientId,
            0,
            *state,
        });
    }
}

} // namespace archstreamer
