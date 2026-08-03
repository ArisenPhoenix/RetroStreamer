#pragma once

#if defined(_WIN32)
#include "host/nds/windows_melonds_runtime.hpp"
#else
#include "host/nds/posix_melonds_runtime.hpp"
#endif

namespace archstreamer {

#if defined(_WIN32)
using MelonDsRuntime = WindowsMelonDsRuntime;
#else
using MelonDsRuntime = PosixMelonDsRuntime;
#endif

} // namespace archstreamer
