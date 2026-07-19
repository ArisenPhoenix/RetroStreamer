#pragma once

#include "common/controller_state.hpp"
#include "common/protocol.hpp"

namespace archstreamer {

class VirtualGamepadBus {
public:
    virtual ~VirtualGamepadBus() = default;

    virtual void plug(RetroArchPort port) = 0;
    virtual void unplug(RetroArchPort port) = 0;
    virtual void update(RetroArchPort port, const ControllerState& state) = 0;
};

} // namespace archstreamer
