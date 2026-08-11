#pragma once

#if defined(_WIN32)
#include "host/hardware/media_capture.hpp"
#include "host/platform/default_save_profile_paths.hpp"
#include "host/hardware/windows_media_server.hpp"
#include "host/console/windows_retroarch_process.hpp"
#include "host/virtual/windows_vigem_gamepad.hpp"
#else
#include "host/hardware/gstreamer_media_server.hpp"
#include "host/virtual/linux_uinput_gamepad.hpp"
#include "host/console/posix_retroarch_process.hpp"
#endif

#include "host/hardware/media_server.hpp"
#include "host/platform/host_pad_platform.hpp"
#include "host/virtual/virtual_keyboard.hpp"

#include <memory>

namespace archstreamer {

#if defined(_WIN32)
using HostRetroArchProcess = WindowsRetroArchProcess;
using HostVirtualGamepadBus = ViGEmGamepadBus;
using HostMediaServer = WindowsMediaServer;
using HostSaveProfilePaths = SaveProfilePaths;

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

// Shared concrete type; Linux XTest vs Windows SendInput selected by CMake sources.
using HostVirtualKeyboard = VirtualKeyboard;

} // namespace archstreamer
