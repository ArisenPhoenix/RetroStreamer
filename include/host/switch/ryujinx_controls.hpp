#pragma once

#include "host/standalone_emulator.hpp"
#include "host/virtual_joypad_resolve.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace archstreamer {

class RyujinxControls {
public:
    static void configure_archstreamer_controls(
        RyujinxUserProfile& profile,
        const std::vector<ArchStreamerSdlPad>& pads,
        const std::string& sdl_device_filter = {});

    /**
     * If Config.json lost GamepadSDL2 (Ryujinx often falls back to WindowKeyboard
     * when the pad was grabbed at startup), rewrite ArchStreamer bindings.
     * Returns true when a rewrite was performed.
     */
    static bool reassert_archstreamer_controls_if_needed(
        const std::filesystem::path& data_root,
        const std::vector<ArchStreamerSdlPad>& pads,
        const std::string& sdl_device_filter = {});
};

} // namespace archstreamer
