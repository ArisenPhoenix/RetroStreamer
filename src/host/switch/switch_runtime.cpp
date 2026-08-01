#include "host/switch/switch_runtime.hpp"

#include "host/switch/default_switch_platform.hpp"

namespace archstreamer {

std::optional<ResolvedStandaloneEmulator> SwitchRuntime::resolve() {
    if (RyujinxRuntime::available()) {
        return RyujinxRuntime::ensure();
    }
    if (YuzuRuntime::available()) {
        return YuzuRuntime::ensure();
    }
    return std::nullopt;
}

std::string SwitchRuntime::unavailable_message() {
    return "No Switch emulator found. Install Ryujinx (preferred) under " +
        RyujinxRuntime::runtime_root().string() + " or Yuzu under " +
        YuzuRuntime::runtime_root().string() +
        " (or set ARCHSTREAMER_RYUJINX / ARCHSTREAMER_YUZU).";
}

} // namespace archstreamer
