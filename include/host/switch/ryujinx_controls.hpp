#pragma once

#include "host/standalone_emulator.hpp"

#include <string>
#include <vector>

namespace archstreamer {

class RyujinxControls {
public:
    static void configure_archstreamer_controls(
        RyujinxUserProfile& profile,
        const std::vector<std::string>& sdl_guids,
        const std::vector<std::string>& mapping_guids = {});
};

} // namespace archstreamer
