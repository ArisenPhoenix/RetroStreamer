#pragma once

#include <string>
#include <vector>

namespace archstreamer {

struct ResolvedRetroArch {
    // Full argv prefix before RetroArch flags (-f, -L, …). Includes the executable.
    std::vector<std::string> argv_prefix;
    std::string display_path;
};

// Prefer a native retroarch binary; otherwise Flatpak org.libretro.RetroArch.
ResolvedRetroArch resolve_retroarch();

} // namespace archstreamer
