#pragma once

#include "host/standalone_emulator.hpp"

#include <filesystem>
#include <optional>
#include <string>

namespace archstreamer {

class WindowsMelonDsRuntime {
public:
    static std::filesystem::path runtime_root();
    static bool available();
    static std::string unavailable_message();
    static std::optional<ResolvedStandaloneEmulator> ensure();
    static std::optional<std::filesystem::path> find_source_binary();
};

} // namespace archstreamer
