#pragma once

#include "host/standalone_emulator.hpp"
#include "host/virtual_joypad_resolve.hpp"

#include <string>
#include <vector>

namespace archstreamer {

class RyujinxControls {
public:
    static void configure_archstreamer_controls(
        RyujinxUserProfile& profile,
        const std::vector<ArchStreamerSdlPad>& pads,
        const std::string& sdl_device_filter = {});
};

} // namespace archstreamer
