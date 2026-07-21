#pragma once

#if defined(_WIN32)
#error "Windows host platform implementation has not been added yet"
#else
#include "host/gstreamer_media_server.hpp"
#include "host/linux_uinput_gamepad.hpp"
#include "host/posix_retroarch_process.hpp"
#endif

#include "host/media_server.hpp"

#include <memory>

namespace archstreamer {

#if !defined(_WIN32)
using HostRetroArchProcess = PosixRetroArchProcess;
using HostVirtualGamepadBus = LinuxUinputGamepadBus;
using HostMediaServer = GStreamerMediaServer;

inline std::unique_ptr<MediaServer> make_host_media_server(const GStreamerMediaCaptureConfig& config) {
    return make_gstreamer_media_server(config);
}
#endif

} // namespace archstreamer
