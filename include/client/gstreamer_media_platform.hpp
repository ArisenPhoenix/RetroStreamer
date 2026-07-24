#pragma once

#if defined(_WIN32)
#include "client/windows_gstreamer_media_platform.hpp"
#else
#include "client/posix_gstreamer_media_platform.hpp"
#endif

namespace archstreamer {

#if defined(_WIN32)
using GStreamerMediaPlatform = WindowsGStreamerMediaPlatform;
#else
using GStreamerMediaPlatform = PosixGStreamerMediaPlatform;
#endif

} // namespace archstreamer
