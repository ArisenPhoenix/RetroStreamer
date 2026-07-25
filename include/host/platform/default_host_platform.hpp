#pragma once

#if defined(_WIN32)
#include "host/media_capture.hpp"
#include "host/windows_media_server.hpp"
#include "host/windows_retroarch_process.hpp"
#include "host/windows_vigem_gamepad.hpp"
#else
#include "host/gstreamer_media_server.hpp"
#include "host/linux_uinput_gamepad.hpp"
#include "host/posix_retroarch_process.hpp"
#endif

#include "host/media_server.hpp"

#include <memory>

namespace archstreamer {

#if defined(_WIN32)
using HostRetroArchProcess = WindowsRetroArchProcess;
using HostVirtualGamepadBus = ViGEmGamepadBus;
using HostMediaServer = WindowsMediaServer;

inline std::unique_ptr<MediaServer> make_host_media_server(const GStreamerMediaCaptureConfig& config) {
    WindowsMediaCaptureConfig win{};
    win.video = config.video;
    win.audio = config.audio;
    win.video_resolution = config.video_resolution;
    win.verbose = config.verbose;
    win.nvenc_cuda_device_id = config.nvenc_cuda_device_id;
    return make_windows_media_server(win);
}
#else
using HostRetroArchProcess = PosixRetroArchProcess;
using HostVirtualGamepadBus = LinuxUinputGamepadBus;
using HostMediaServer = GStreamerMediaServer;

inline std::unique_ptr<MediaServer> make_host_media_server(const GStreamerMediaCaptureConfig& config) {
    return make_gstreamer_media_server(config);
}
#endif

} // namespace archstreamer
