#pragma once

#if defined(_WIN32)
#include "host/console/nds/windows_melonds_runtime.hpp"
#else
#include "host/console/nds/posix_melonds_runtime.hpp"
#endif

namespace archstreamer {

#if defined(_WIN32)
using MelonDsRuntime = WindowsMelonDsRuntime;
#else
using MelonDsRuntime = PosixMelonDsRuntime;
#endif

} // namespace archstreamer
