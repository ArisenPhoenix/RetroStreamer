#pragma once

#include "host/standalone_emulator.hpp"

#include <string>
#include <vector>

namespace archstreamer {

class YuzuControls {
public:
    static void configure_archstreamer_controls(
        const YuzuUserProfile& profile,
        const std::vector<std::string>& sdl_guids);
};

} // namespace archstreamer
