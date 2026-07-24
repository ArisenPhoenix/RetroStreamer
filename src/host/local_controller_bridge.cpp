#include "host/host_session_helpers.hpp"
#include "host/local_controller_bridge.hpp"
#include "common/platform/process_utils.hpp"

#include <SDL.h>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>

namespace archstreamer {
namespace {

void clear_sdl_controller_ignore() {
    // RetroArch gets IGNORE via its own child env; the host bridge must see the real pad.
    // Clear both the process env and the SDL hint before any ControllerBackend SDL_Init.
    unsetenv("SDL_GAMECONTROLLER_IGNORE_DEVICES");
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES, "");
}

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
    clear_sdl_controller_ignore();
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
    clear_sdl_controller_ignore();

    // Best-effort: if a libvirt guest still has this pad via virtio-evdev, QEMU holds an
    // exclusive grab and Host Player gets no events. Detach common default VM name.
    (void)archstreamer::read_command_output(
        "command -v virsh >/dev/null 2>&1 && "
        "(sg libvirt -c 'virsh domstate archstreamer-client' 2>/dev/null | grep -q running) && "
        "sg libvirt -c \"virsh dumpxml archstreamer-client\" 2>/dev/null | "
        "python3 -c \"import sys,re; "
        "m=re.search(r\\\"<input type='passthrough'.*?</input>\\\",sys.stdin.read(),re.S); "
        "open('/tmp/archstreamer-evdev-detach.xml','w').write(m.group(0) if m else '')\" && "
        "test -s /tmp/archstreamer-evdev-detach.xml && "
        "sg libvirt -c 'virsh detach-device archstreamer-client /tmp/archstreamer-evdev-detach.xml --live' "
        "2>/dev/null && "
        "echo 'Detached virtio-evdev pad from archstreamer-client for Host Player.'");

    clear_sdl_controller_ignore();

    // Re-resolve by GUID/vid/pid. The SDL index captured at selection time is stale once
    // virtual ArchStreamer pads appear (and must not run under the RetroArch ignore list).
    const auto live_id = resolve_live_device_id(device_);
    std::cout
        << "Bridging local controller "
        << ": " << device_.name
        << " [selected id=" << device_.id << " live id=" << live_id << "]"
        << " vid/pid=" << hex_vid_pid(device_.vendor_id, device_.product_id)
        << "\n";

    // Construct the long-lived backend only after IGNORE is cleared so SDL_Init sees
    // the physical pad (member default-init would race with a stale ignore hint).
    backend_.emplace();
    backend_->open_selected({live_id});
}

const ControllerDevice& LocalControllerBridge::device() const {
    return device_;
}

void LocalControllerBridge::update(InputRouter& input_router) {
    if (!backend_.has_value()) {
        return;
    }
    const auto state = backend_->poll(0);
    if (!state.has_value()) {
        return;
    }
    if (!input_router.route(ControllerInput{
            HostClientId,
            0,
            *state,
        })) {
        static bool logged_missing_seat = false;
        if (!logged_missing_seat) {
            logged_missing_seat = true;
            std::cerr
                << "Host bridge input has no seat assignment; "
                << "virtual pad will not receive Host Player controls\n";
        }
    }
}

} // namespace archstreamer
