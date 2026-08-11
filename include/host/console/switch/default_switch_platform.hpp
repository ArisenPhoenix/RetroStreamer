#pragma once

#include "host/console/switch/default_switch_paths.hpp"

#if defined(_WIN32)
#include "host/console/switch/windows_ryujinx_runtime.hpp"
#include "host/console/switch/windows_yuzu_runtime.hpp"
#else
#include "host/console/switch/posix_ryujinx_runtime.hpp"
#include "host/console/switch/posix_yuzu_runtime.hpp"
#endif

namespace archstreamer {

#if defined(_WIN32)
using YuzuRuntime = WindowsYuzuRuntime;
using RyujinxRuntime = WindowsRyujinxRuntime;
#else
using YuzuRuntime = PosixYuzuRuntime;
using RyujinxRuntime = PosixRyujinxRuntime;
#endif

} // namespace archstreamer
