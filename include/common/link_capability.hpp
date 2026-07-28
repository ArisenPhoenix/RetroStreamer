#pragma once

#include <string_view>

namespace archstreamer {

/** Systems that can participate in ArchStreamer "link" (cable / LDN / etc.). */
inline bool system_supports_link(std::string_view system_key) {
    return system_key == "switch" ||
        system_key == "gb" ||
        system_key == "gbc" ||
        system_key == "gb-gbc" ||
        system_key == "gba";
}

} // namespace archstreamer
