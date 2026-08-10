#pragma once

#include "common/protocol.hpp"
#include "host/capture_platform.hpp"
#include "host/gpu_select.hpp"
#include "host/host_app_config.hpp"
#include "host/host_launch_planner.hpp"
#include "host/launch_environment.hpp"
#include "host/media_server.hpp"
#include "host/nds/melonds_backend.hpp"
#include "host/pad_plan.hpp"
#include "host/soft_keyboard_host.hpp"
#include "host/switch/switch_backend.hpp"
#include "host/virtual_gamepad.hpp"
#include "host/virtual_joypad_resolve.hpp"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace archstreamer {

struct SessionGpuSelection {
    std::optional<GpuDevice> resolved_encode;
    std::optional<GpuDevice> resolved_gpu;
    std::string gamescope_vk_device;
    int nvenc_cuda_device_id = -1;
};

struct SessionLaunchEnvironment {
    CapturePlan capture;
    EmulatorLaunchEnvRequest request;
    SessionGpuSelection gpu;
};

struct SessionMediaPlan {
    HostMediaPlanConfig config;
    std::vector<HostMediaDestination> destinations;
    std::vector<MediaClientStream> streams;
    std::uint16_t capture_w = 1920;
    std::uint16_t capture_h = 1080;
};

struct SessionBackendState {
    std::unique_ptr<SwitchBackend> switch_backend;
    std::unique_ptr<MelonDsBackend> melonds_backend;
    std::string switch_launch_content_stem;
    std::string switch_launch_title_id;
    std::string system_key;
};

struct SessionDevicePlan {
    PadPlan shared_pad_plan;
    std::vector<std::size_t> resolved_indices;
    std::vector<ArchStreamerSdlPad> resolved_pads;
    std::size_t virtual_joypad_index = 0;
    std::uint16_t product_id_base = 0;
    std::string soft_keyboard_fallback;
    std::shared_ptr<SoftKeyboardHostBridge> standalone_soft_keyboard;
    std::optional<GpuDevice> resolved_gpu;
    bool arm_soft_keyboard = false;
    bool use_virtual_capture = false;
    bool capture_fullscreen = false;
    std::string capture_display;
    VirtualDisplayBackend display_backend = VirtualDisplayBackend::None;
    std::string video_resolution;

    std::string capture_info() const;
};

void apply_capture_to_session_device_plan(
    SessionDevicePlan& devices,
    const HostAppConfig& config,
    const CapturePlan& capture,
    std::optional<GpuDevice> resolved_gpu = std::nullopt);

void plug_session_gamepads(VirtualGamepadBus& gamepads, RetroArchPort players);

void wait_for_session_input_enumeration();

} // namespace archstreamer
