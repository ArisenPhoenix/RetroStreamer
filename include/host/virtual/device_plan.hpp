#pragma once

#include "client/controller_manager.hpp"
#include "host/virtual/pad_plan.hpp"
#include "host/virtual/pad_settings.hpp"
#include "host/virtual/soft_keyboard_host.hpp"
#include "host/virtual/virtual_gamepad.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace archstreamer {

struct SessionInputDevices {
    PadPlan shared_pad_plan;
    std::vector<std::size_t> resolved_indices;
    std::vector<ArchStreamerSdlPad> resolved_pads;
    std::size_t virtual_joypad_index = 0;
    std::uint16_t product_id_base = 0;
};

struct SessionKeyboardDevices {
    std::string soft_keyboard_fallback;
    std::shared_ptr<SoftKeyboardHostBridge> standalone_soft_keyboard;
    bool arm_soft_keyboard = false;
};

struct SessionTouchDevices {
};

struct SessionDevicePlan {
    SessionInputDevices input;
    SessionKeyboardDevices keyboard;
    SessionTouchDevices touch;
};

enum class SessionPadPlanKind {
    Direct,
    RetroArchSlot,
};

SessionDevicePlan resolve_session_device_plan(
    const PadSettings& pads,
    bool verbose,
    std::uint8_t players,
    SessionPadPlanKind kind,
    std::uint16_t product_id_base);

void plug_session_gamepads(VirtualGamepadBus& gamepads, RetroArchPort players);

void wait_for_session_input_enumeration();

void append_session_controller_ignore_list(
    PadSettings& pads,
    const std::optional<ControllerDevice>& bridge_device = std::nullopt,
    std::string_view log_prefix = {},
    bool warn_if_not_sdl2 = false);

} // namespace archstreamer
