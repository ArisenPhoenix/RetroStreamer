#pragma once

#if defined(_WIN32)
#include "host/switch/windows_switch_paths.hpp"
#else
#include "host/switch/posix_switch_paths.hpp"
#endif

namespace archstreamer {

#if defined(_WIN32)
using SwitchPaths = WindowsSwitchPaths;
#else
using SwitchPaths = PosixSwitchPaths;
#endif

} // namespace archstreamer
