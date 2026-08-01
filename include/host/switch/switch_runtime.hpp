#pragma once

#include "host/standalone_emulator.hpp"

#include <optional>
#include <string>

namespace archstreamer {

class SwitchRuntime {
public:
    static std::optional<ResolvedStandaloneEmulator> resolve();
    static std::string unavailable_message();
};

} // namespace archstreamer
