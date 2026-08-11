#pragma once

#include "common/controller_state.hpp"
#include "common/protocol.hpp"

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

class VirtualGamepadBus {
public:
    virtual ~VirtualGamepadBus() = default;

    virtual void plug(RetroArchPort port) = 0;
    virtual void unplug(RetroArchPort port) = 0;
    virtual void update(RetroArchPort port, const ControllerState& state) = 0;
};

} // namespace archstreamer
