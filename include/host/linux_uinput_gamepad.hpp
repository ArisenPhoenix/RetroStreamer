#pragma once

#include "host/virtual_gamepad.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace archstreamer {

struct VirtualGamepadIdentity {
    std::string name = "ArchStreamer Virtual Gamepad";
    std::uint16_t vendor_id = 0x1209;
    std::uint16_t product_id = 0xa517;
    std::uint16_t version = 1;
};

class LinuxUinputGamepadBus final : public VirtualGamepadBus {
public:
    explicit LinuxUinputGamepadBus(VirtualGamepadIdentity identity = {});
    explicit LinuxUinputGamepadBus(std::vector<VirtualGamepadIdentity> identities);
    ~LinuxUinputGamepadBus() override;

    LinuxUinputGamepadBus(const LinuxUinputGamepadBus&) = delete;
    LinuxUinputGamepadBus& operator=(const LinuxUinputGamepadBus&) = delete;

    void plug(RetroArchPort port) override;
    void unplug(RetroArchPort port) override;
    void update(RetroArchPort port, const ControllerState& state) override;

private:
    struct Pad {
        int fd = -1;
        bool plugged = false;
        // Last emitted state. uinput ABS writes are delivered even when unchanged;
        // re-emitting every poll (~8ms) makes RetroArch menus treat holds as many taps.
        bool has_last = false;
        ControllerState last{};
    };

    Pad& pad_for(RetroArchPort port);
    const Pad& pad_for(RetroArchPort port) const;
    VirtualGamepadIdentity identity_for(RetroArchPort port) const;

    std::array<Pad, MaxRetroArchPorts> pads_;
    VirtualGamepadIdentity identity_;
    std::vector<VirtualGamepadIdentity> identities_;
};

} // namespace archstreamer
