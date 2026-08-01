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
    int resolution_scale = 1,
    int slot_index = 0,
    std::uint16_t network_cmd_port = 55355);

// Which face-button mapping write_retroarch_input_override() applies, for logging.
std::string_view face_button_map_name(std::string_view system_key);

// True for HW-rendered libretro cores that need gl + (usually) VirtualGL on Xvfb.
// Software cores (gambatte, etc.) should use plain Xvfb + sdl2 — vglrun left remotes
// stuck on static GB credits/title until continuous animation.
bool core_needs_gl_on_virtual_display(const std::filesystem::path& core_path);

} // namespace archstreamer
