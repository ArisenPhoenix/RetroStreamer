#pragma once

#include "host/virtual_gamepad.hpp"
#include "host/save_profile.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace archstreamer {

VirtualGamepadIdentity identity_for_port(
    const std::vector<VirtualGamepadIdentity>& identities,
    RetroArchPort port);

std::filesystem::path write_retroarch_input_override(
    std::size_t first_virtual_joypad_index,
    const std::vector<VirtualGamepadIdentity>& identities,
    const std::string& joypad_driver,
    RetroArchPort players,
    const SaveProfile& save_profile,
    bool realtime_pacing,
    bool capture_fullscreen = false,
    std::string_view capture_resolution = {},
    int vulkan_gpu_index = -1,
    std::string_view system_key = {},
    const std::filesystem::path& core_path = {},
    int resolution_scale = 1);

} // namespace archstreamer
