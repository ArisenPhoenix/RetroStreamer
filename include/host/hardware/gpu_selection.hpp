#pragma once

#include "host/contracts/gpu.hpp"
#include "host/contracts/launch.hpp"
#include "host/contracts/present.hpp"
#include "host/hardware/gpu_settings.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace archstreamer {

struct SessionGpuSelection {
    std::optional<GpuTarget> resolved_encode;
    std::optional<GpuTarget> resolved_gpu;
    std::string gamescope_vk_device;
    int nvenc_cuda_device_id = -1;
};

SessionGpuSelection resolve_session_gpu_selection(
    const GpuSettings& gpu,
    const PresentTarget& present,
    std::string_view log_prefix = {},
    bool detailed_logging = false);

void apply_session_gpu_to_launch_request(
    EmulatorLaunchEnvRequest& request,
    const SessionGpuSelection& selection);

} // namespace archstreamer
