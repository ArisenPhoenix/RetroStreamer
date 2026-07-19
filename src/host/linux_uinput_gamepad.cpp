#include "host/linux_uinput_gamepad.hpp"

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>

#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <linux/uinput.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace archstreamer {
namespace {

void checked_ioctl(int fd, unsigned long request, int value, const char* message) {
    if (ioctl(fd, request, value) < 0) {
        throw std::runtime_error(std::string(message) + ": " + std::strerror(errno));
    }
}

void emit_event(int fd, std::uint16_t type, std::uint16_t code, std::int32_t value) {
    input_event event{};
    event.type = type;
    event.code = code;
    event.value = value;

    if (write(fd, &event, sizeof(event)) != sizeof(event)) {
        throw std::runtime_error(std::string("failed to write uinput event: ") + std::strerror(errno));
    }
}

void setup_abs(uinput_user_dev& device, int axis, int min, int max, int flat = 0) {
    device.absmin[axis] = min;
    device.absmax[axis] = max;
    device.absflat[axis] = flat;
}

void set_button_bit(int fd, int button) {
    checked_ioctl(fd, UI_SET_KEYBIT, button, "failed to set uinput button bit");
}

void emit_button(int fd, const ControllerState& state, ControllerButton source, int target) {
    emit_event(fd, EV_KEY, target, (state.buttons & source) != 0 ? 1 : 0);
}

} // namespace

LinuxUinputGamepadBus::LinuxUinputGamepadBus(VirtualGamepadIdentity identity)
    : identity_(std::move(identity)) {
    if (identity_.name.empty()) {
        identity_.name = "ArchStreamer Virtual Gamepad";
    }
}

LinuxUinputGamepadBus::LinuxUinputGamepadBus(std::vector<VirtualGamepadIdentity> identities)
    : identities_(std::move(identities)) {
    if (identities_.empty()) {
        identity_.name = "ArchStreamer Virtual Gamepad";
    }
}

LinuxUinputGamepadBus::~LinuxUinputGamepadBus() {
    for (RetroArchPort port = 0; port < pads_.size(); ++port) {
        unplug(port);
    }
}

void LinuxUinputGamepadBus::plug(RetroArchPort port) {
    auto& pad = pad_for(port);
    if (pad.plugged) {
        return;
    }

    const int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        throw std::runtime_error(std::string("failed to open /dev/uinput: ") + std::strerror(errno));
    }

    try {
        checked_ioctl(fd, UI_SET_EVBIT, EV_KEY, "failed to enable uinput key events");
        checked_ioctl(fd, UI_SET_EVBIT, EV_ABS, "failed to enable uinput absolute axis events");

        set_button_bit(fd, BTN_SOUTH);
        set_button_bit(fd, BTN_EAST);
        set_button_bit(fd, BTN_WEST);
        set_button_bit(fd, BTN_NORTH);
        set_button_bit(fd, BTN_SELECT);
        set_button_bit(fd, BTN_MODE);
        set_button_bit(fd, BTN_START);
        set_button_bit(fd, BTN_THUMBL);
        set_button_bit(fd, BTN_THUMBR);
        set_button_bit(fd, BTN_TL);
        set_button_bit(fd, BTN_TR);
        set_button_bit(fd, BTN_DPAD_UP);
        set_button_bit(fd, BTN_DPAD_DOWN);
        set_button_bit(fd, BTN_DPAD_LEFT);
        set_button_bit(fd, BTN_DPAD_RIGHT);

        checked_ioctl(fd, UI_SET_ABSBIT, ABS_X, "failed to set ABS_X");
        checked_ioctl(fd, UI_SET_ABSBIT, ABS_Y, "failed to set ABS_Y");
        checked_ioctl(fd, UI_SET_ABSBIT, ABS_RX, "failed to set ABS_RX");
        checked_ioctl(fd, UI_SET_ABSBIT, ABS_RY, "failed to set ABS_RY");
        checked_ioctl(fd, UI_SET_ABSBIT, ABS_Z, "failed to set ABS_Z");
        checked_ioctl(fd, UI_SET_ABSBIT, ABS_RZ, "failed to set ABS_RZ");
        checked_ioctl(fd, UI_SET_ABSBIT, ABS_HAT0X, "failed to set ABS_HAT0X");
        checked_ioctl(fd, UI_SET_ABSBIT, ABS_HAT0Y, "failed to set ABS_HAT0Y");

        const auto identity = identity_for(port);

        uinput_user_dev device{};
        std::snprintf(device.name, UINPUT_MAX_NAME_SIZE, "%s", identity.name.c_str());
        device.id.bustype = BUS_USB;
        device.id.vendor = identity.vendor_id;
        device.id.product = identity.product_id;
        device.id.version = identity.version;

        setup_abs(device, ABS_X, -32768, 32767, 8000);
        setup_abs(device, ABS_Y, -32768, 32767, 8000);
        setup_abs(device, ABS_RX, -32768, 32767, 8000);
        setup_abs(device, ABS_RY, -32768, 32767, 8000);
        setup_abs(device, ABS_Z, 0, 65535);
        setup_abs(device, ABS_RZ, 0, 65535);
        setup_abs(device, ABS_HAT0X, -1, 1);
        setup_abs(device, ABS_HAT0Y, -1, 1);

        if (write(fd, &device, sizeof(device)) != sizeof(device)) {
            throw std::runtime_error(std::string("failed to configure uinput device: ") + std::strerror(errno));
        }
        checked_ioctl(fd, UI_DEV_CREATE, 0, "failed to create uinput device");

        pad.fd = fd;
        pad.plugged = true;
    } catch (...) {
        close(fd);
        throw;
    }
}

void LinuxUinputGamepadBus::unplug(RetroArchPort port) {
    auto& pad = pad_for(port);
    if (!pad.plugged) {
        return;
    }

    ioctl(pad.fd, UI_DEV_DESTROY);
    close(pad.fd);
    pad.fd = -1;
    pad.plugged = false;
}

void LinuxUinputGamepadBus::update(RetroArchPort port, const ControllerState& state) {
    auto& pad = pad_for(port);
    if (!pad.plugged) {
        plug(port);
    }

    emit_event(pad.fd, EV_ABS, ABS_X, state.left_x);
    emit_event(pad.fd, EV_ABS, ABS_Y, state.left_y);
    emit_event(pad.fd, EV_ABS, ABS_RX, state.right_x);
    emit_event(pad.fd, EV_ABS, ABS_RY, state.right_y);
    emit_event(pad.fd, EV_ABS, ABS_Z, state.left_trigger);
    emit_event(pad.fd, EV_ABS, ABS_RZ, state.right_trigger);

    emit_event(pad.fd, EV_ABS, ABS_HAT0X,
        (state.buttons & ButtonDpadRight ? 1 : 0) - (state.buttons & ButtonDpadLeft ? 1 : 0));
    emit_event(pad.fd, EV_ABS, ABS_HAT0Y,
        (state.buttons & ButtonDpadDown ? 1 : 0) - (state.buttons & ButtonDpadUp ? 1 : 0));

    emit_button(pad.fd, state, ButtonA, BTN_SOUTH);
    emit_button(pad.fd, state, ButtonB, BTN_EAST);
    emit_button(pad.fd, state, ButtonX, BTN_WEST);
    emit_button(pad.fd, state, ButtonY, BTN_NORTH);
    emit_button(pad.fd, state, ButtonBack, BTN_SELECT);
    emit_button(pad.fd, state, ButtonGuide, BTN_MODE);
    emit_button(pad.fd, state, ButtonStart, BTN_START);
    emit_button(pad.fd, state, ButtonLeftStick, BTN_THUMBL);
    emit_button(pad.fd, state, ButtonRightStick, BTN_THUMBR);
    emit_button(pad.fd, state, ButtonLeftShoulder, BTN_TL);
    emit_button(pad.fd, state, ButtonRightShoulder, BTN_TR);
    emit_button(pad.fd, state, ButtonDpadUp, BTN_DPAD_UP);
    emit_button(pad.fd, state, ButtonDpadDown, BTN_DPAD_DOWN);
    emit_button(pad.fd, state, ButtonDpadLeft, BTN_DPAD_LEFT);
    emit_button(pad.fd, state, ButtonDpadRight, BTN_DPAD_RIGHT);
    emit_event(pad.fd, EV_SYN, SYN_REPORT, 0);
}

LinuxUinputGamepadBus::Pad& LinuxUinputGamepadBus::pad_for(RetroArchPort port) {
    if (port >= pads_.size()) {
        throw std::runtime_error("invalid RetroArch port");
    }

    return pads_[port];
}

const LinuxUinputGamepadBus::Pad& LinuxUinputGamepadBus::pad_for(RetroArchPort port) const {
    if (port >= pads_.size()) {
        throw std::runtime_error("invalid RetroArch port");
    }

    return pads_[port];
}

VirtualGamepadIdentity LinuxUinputGamepadBus::identity_for(RetroArchPort port) const {
    if (port < identities_.size()) {
        auto identity = identities_[port];
        if (identity.name.empty()) {
            identity.name = "ArchStreamer Virtual Gamepad";
        }
        identity.name += " P" + std::to_string(static_cast<int>(port) + 1);
        identity.product_id = static_cast<std::uint16_t>(identity.product_id + port);
        return identity;
    }

    auto identity = identity_;
    identity.name += " P" + std::to_string(static_cast<int>(port) + 1);
    identity.product_id = static_cast<std::uint16_t>(identity.product_id + port);
    return identity;
}

} // namespace archstreamer
