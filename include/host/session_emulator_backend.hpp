#pragma once

#include "host/retroarch_netcmd.hpp"
#include "host/session_launch_types.hpp"

#include <cstdint>
#include <memory>

namespace archstreamer {

struct SoftKeyboardHostBridge;
class VirtualKeyboard;

struct SessionBackendPrepareOptions {
    int slot_index = 0;
    bool use_virtual_capture = false;
    bool gamescope_capture = false;
    bool log_retroarch_details = false;
    bool prefer_switch_handheld_mode = false;
    std::uint16_t retroarch_netcmd_port = DefaultRetroArchNetcmdPort;
    VirtualKeyboard* keyboard = nullptr;
};

struct SessionBackendPrepareResult {
    std::shared_ptr<SoftKeyboardHostBridge> soft_keyboard;
};

class SessionEmulatorBackend {
public:
    virtual ~SessionEmulatorBackend() = default;

    virtual const char* name() const = 0;
    virtual SessionBackendPrepareResult prepare(
        SessionBackendPrepareContext& context,
        const SessionBackendPrepareOptions& options) = 0;
};

SessionBackendPrepareResult prepare_session_emulator_backend(
    SessionBackendPrepareContext& context,
    const SessionBackendPrepareOptions& options);

} // namespace archstreamer
