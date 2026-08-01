#pragma once

#include "host/standalone_emulator.hpp"

#include <string>
#include <utility>
#include <vector>

namespace archstreamer {

class YuzuLaunchEnv {
public:
    static std::vector<std::pair<std::string, std::string>> launch_environment(
        const YuzuUserProfile& profile);
};

} // namespace archstreamer
