#pragma once

#if defined(_WIN32)
#include "host/contracts/media.hpp"
#include "host/platform/default_save_profile_paths.hpp"
#include "host/video/media_config.hpp"
#include "host/video/windows_media_server.hpp"
#include "host/console/windows_retroarch_process.hpp"
#include "host/virtual/windows_vigem_gamepad.hpp"
#else
#include "host/video/gstreamer_media_server.hpp"
#include "host/video/media_config.hpp"
#include "host/virtual/linux_uinput_gamepad.hpp"
#include "host/console/posix_retroarch_process.hpp"
#endif

#include "host/video/media_server.hpp"
#include "host/platform/host_pad_platform.hpp"
#include "host/virtual/virtual_keyboard.hpp"

#include <memory>

namespace archstreamer {

#if defined(_WIN32)
using HostRetroArchProcess = WindowsRetroArchProcess;
using HostVirtualGamepadBus = ViGEmGamepadBus;
using HostMediaServer = WindowsMediaServer;
using HostSaveProfilePaths = SaveProfilePaths;

inline std::unique_ptr<MediaServer> make_host_media_server(
    const MediaConfig& media,
    const PresentTarget& present,
    bool verbose,
    int nvenc_cuda_device_id) {
    auto capture = resolve_media_capture(media, present, verbose, nvenc_cuda_device_id);
    WindowsMediaCaptureConfig win{};
    win.video = capture.video;
    win.audio = capture.audio;
    win.video_resolution = capture.video_resolution;
    win.verbose = capture.verbose;
    win.nvenc_cuda_device_id = capture.nvenc_cuda_device_id;
    return make_windows_media_server(win);
}
#else
using HostRetroArchProcess = PosixRetroArchProcess;
using HostVirtualGamepadBus = LinuxUinputGamepadBus;
using HostMediaServer = GStreamerMediaServer;

inline std::unique_ptr<MediaServer> make_host_media_server(
    const MediaConfig& media,
    const PresentTarget& present,
    bool verbose,
    int nvenc_cuda_device_id) {
    return make_gstreamer_media_server(
        resolve_media_capture(media, present, verbose, nvenc_cuda_device_id));
}
#endif

// Shared concrete type; Linux XTest vs Windows SendInput selected by CMake sources.
using HostVirtualKeyboard = VirtualKeyboard;

} // namespace archstreamer
