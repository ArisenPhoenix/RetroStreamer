#pragma once

#include "host/platform/host_pad_platform.hpp"

#include <cstddef>
#include <optional>
#include <string>

namespace archstreamer {

/**
 * Host-facing pad knobs. Virtual owns this; HostAppConfig embeds it so session
 * resolves seats from here instead of loose HostAppConfig fields.
 */
struct PadSettings {
    bool pulse_input = false;
    std::optional<std::size_t> bridge_controller_index;
    std::optional<std::size_t> virtual_joypad_index;
    std::optional<std::string> ignore_controller;
    std::string retroarch_joypad_driver = kDefaultRetroArchJoypadDriver;
};

} // namespace archstreamer
