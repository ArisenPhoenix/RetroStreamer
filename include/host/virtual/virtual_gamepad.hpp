#pragma once

#include "common/controller_state.hpp"
#include "common/protocol.hpp"

#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace archstreamer {

struct VirtualGamepadIdentity {
    std::string name = "ArchStreamer Virtual Gamepad";
    std::uint16_t vendor_id = 0x1209;
    std::uint16_t product_id = 0xa517;
    std::uint16_t version = 1;
};

/** SDL/udev ignore-list dialect (`0xvvvv/0xpppp`). Cadence stores the ints. */
inline std::string hex_vid_pid(std::uint16_t vendor_id, std::uint16_t product_id) {
    std::ostringstream out;
    out
        << "0x" << std::hex << std::setw(4) << std::setfill('0') << vendor_id
        << "/0x" << std::hex << std::setw(4) << std::setfill('0') << product_id;
    return out.str();
}

class VirtualGamepadBus {
public:
    virtual ~VirtualGamepadBus() = default;

    virtual void plug(RetroArchPort port) = 0;
    virtual void unplug(RetroArchPort port) = 0;
    virtual void update(RetroArchPort port, const ControllerState& state) = 0;
};

} // namespace archstreamer
