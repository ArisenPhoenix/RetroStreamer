#include "host/host_session_helpers.hpp"
#include "host/local_controller_bridge.hpp"

#include <chrono>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace archstreamer {

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
    std::cout
        << "Bridging local controller "
        << ": " << device_.name
        << " [id=" << device_.id << "]"
        << " vid/pid=" << hex_vid_pid(device_.vendor_id, device_.product_id)
        << "\n";

    backend_.open_selected({device_.id});
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
