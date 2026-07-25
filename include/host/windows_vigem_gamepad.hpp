#pragma once

#include "host/virtual_gamepad.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace archstreamer {

// Xbox 360 pads via ViGEmBus (ViGEmClient.dll loaded at runtime).
class ViGEmGamepadBus final : public VirtualGamepadBus {
public:
    explicit ViGEmGamepadBus(VirtualGamepadIdentity identity = {});
    explicit ViGEmGamepadBus(std::vector<VirtualGamepadIdentity> identities);
    ~ViGEmGamepadBus() override;

    ViGEmGamepadBus(const ViGEmGamepadBus&) = delete;
    ViGEmGamepadBus& operator=(const ViGEmGamepadBus&) = delete;

    void plug(RetroArchPort port) override;
    void unplug(RetroArchPort port) override;
    void update(RetroArchPort port, const ControllerState& state) override;

private:
    struct Pad {
        void* target = nullptr; // PVIGEM_TARGET
        bool plugged = false;
        bool has_last = false;
        ControllerState last{};
    };

    void ensure_client();
    Pad& pad_for(RetroArchPort port);

    void* client_ = nullptr; // PVIGEM_CLIENT
    std::array<Pad, MaxRetroArchPorts> pads_{};
    VirtualGamepadIdentity identity_{};
    std::vector<VirtualGamepadIdentity> identities_{};
};

} // namespace archstreamer
