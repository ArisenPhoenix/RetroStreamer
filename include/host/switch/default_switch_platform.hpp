#pragma once

#include "host/switch/default_switch_paths.hpp"

#if defined(_WIN32)
#include "host/switch/windows_ryujinx_runtime.hpp"
#include "host/switch/windows_yuzu_runtime.hpp"
#else
#include "host/switch/posix_ryujinx_runtime.hpp"
#include "host/switch/posix_yuzu_runtime.hpp"
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
