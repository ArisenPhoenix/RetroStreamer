#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace archstreamer {

/**
 * Host virtual-pad platform tags shared by joypad resolve, RetroArch joypad
 * driver defaults, and SDL_GAMECONTROLLERCONFIG mappings (Ryujinx/Yuzu).
 *
 * Linux uinput + udev remain the reference behaviour in virtual_joypad_resolve.cpp
 * / host_app.cpp. Windows fills the same surface via ViGEm identity + xinput/sdl2.
 */
#if defined(_WIN32)
inline constexpr const char* kSdlGameControllerPlatform = "Windows";
inline constexpr const char* kDefaultRetroArchJoypadDriver = "xinput";
#else
inline constexpr const char* kSdlGameControllerPlatform = "Linux";
inline constexpr const char* kDefaultRetroArchJoypadDriver = "udev";
#endif

/** True when this SDL joystick index is an ArchStreamer host virtual pad. */
bool platform_is_host_virtual_joystick(
    int device_index,
    std::uint16_t product_id_base,
    std::size_t players);

} // namespace archstreamer
