#pragma once

#include <string>

namespace archstreamer {

// Portable user-facing paths. Prefer these over getenv("HOME") / XDG_*.
std::string user_home_directory();
std::string user_cache_directory();
std::string archstreamer_cache_directory();
std::string current_username();

} // namespace archstreamer
