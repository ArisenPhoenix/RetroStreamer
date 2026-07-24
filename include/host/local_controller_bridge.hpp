#pragma once

#include "client/controller_backend.hpp"
#include "client/controller_manager.hpp"
#include "common/protocol.hpp"
#include "host/input_router.hpp"
#include "host/virtual_gamepad.hpp"

#include <cstddef>
#include <optional>

namespace archstreamer {

void pulse_virtual_pad_a(VirtualGamepadBus& gamepads);
ControllerDevice local_bridge_device_for(std::size_t selected_index);

class LocalControllerBridge {
public:
    explicit LocalControllerBridge(ControllerDevice device);

    const ControllerDevice& device() const;
    void update(InputRouter& input_router);

private:
    // Optional so SDL_Init runs only after IGNORE env/hint are cleared.
    std::optional<ControllerBackend> backend_;
    ControllerDevice device_;
};

} // namespace archstreamer
