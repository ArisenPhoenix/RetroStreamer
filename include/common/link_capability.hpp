#pragma once

#include <string_view>

namespace archstreamer {

/** Systems that can participate in ArchStreamer "link" (cable / LDN / etc.). */
inline bool system_supports_link(std::string_view system_key) {
    if (system_key == "gba" ||
        system_key == "nds" ||
        system_key == "switch") {
        return true;
    }
#if defined(ARCHSTREAMER_DEBUG_GB_LINK)
    // Experimental DoubleCherryGB dual-machine path (enable via -DARCHSTREAMER_DEBUG_GB_LINK=ON).
    return system_key == "gb" ||
        system_key == "gbc" ||
        system_key == "gb-gbc";
#else
    return false;
#endif
}

} // namespace archstreamer
