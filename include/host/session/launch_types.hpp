#pragma once

#include "client/controller_manager.hpp"
#include "common/protocol.hpp"
#include "host/hardware/capture_platform.hpp"
#include "host/hardware/gpu_select.hpp"
#include "host/console/game_catalog.hpp"
#include "host/host_app_config.hpp"
#include "host/host_launch_planner.hpp"
#include "host/hardware/launch_environment.hpp"
#include "host/hardware/media_server.hpp"
#include "host/console/nds/melonds_backend.hpp"
#include "host/virtual/pad_plan.hpp"
#include "host/console/retroarch_resolve.hpp"
#include "host/console/save_profile.hpp"
#include "host/session/launch_assemble.hpp"
#include "host/virtual/soft_keyboard_host.hpp"
#include "host/console/switch/switch_backend.hpp"
#include "host/virtual/virtual_gamepad.hpp"
#include "host/virtual/virtual_joypad_resolve.hpp"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace archstreamer {

struct SessionPlan;

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

struct SessionContentInfo {
    std::string active_display_name;
    std::filesystem::path catalog_content_path;
    std::string m3m_title_id;
    std::vector<std::string> playlist_discs;
    std::string system_key;
};

struct SessionLaunchAssets {
    SaveProfile save_profile;
    RetroArchLaunchConfig launch_config;
    ResolvedRetroArch resolved_retroarch;
    SessionContentInfo content;
};

struct SessionLaunchContext {
    HostLaunchPlan launch_plan;
    SessionLaunchAssets assets;
};

struct SwitchLaunchContent {
    std::string content_stem;
    std::string title_id;
};

struct SessionParticipantContext {
    std::vector<ClientHello> client_hellos;
    std::string profile_display_name;
    DisplayLayoutPreference display_layout = DisplayLayoutPreference::Auto;
};

enum class SessionMediaPlanKind {
    Direct,
    Slot,
};

struct SessionBackendState {
    std::unique_ptr<SwitchBackend> switch_backend;
    std::unique_ptr<MelonDsBackend> melonds_backend;
    std::string switch_launch_content_stem;
    std::string switch_launch_title_id;
    std::string system_key;
};

struct SessionInputDevices {
    PadPlan shared_pad_plan;
    std::vector<std::size_t> resolved_indices;
    std::vector<ArchStreamerSdlPad> resolved_pads;
    std::size_t virtual_joypad_index = 0;
    std::uint16_t product_id_base = 0;
};

struct SessionKeyboardDevices {
    std::string soft_keyboard_fallback;
    std::shared_ptr<SoftKeyboardHostBridge> standalone_soft_keyboard;
    bool arm_soft_keyboard = false;
};

struct SessionTouchDevices {
};

struct SessionCaptureDevices {
    std::optional<GpuDevice> resolved_gpu;
    bool use_virtual_capture = false;
    bool capture_fullscreen = false;
    std::string capture_display;
    VirtualDisplayBackend display_backend = VirtualDisplayBackend::None;
    std::string video_resolution;
};

struct SessionDevicePlan {
    SessionInputDevices input;
    SessionKeyboardDevices keyboard;
    SessionTouchDevices touch;
    SessionCaptureDevices capture;

    std::string capture_info() const;
};

struct SessionUserContext {
    SaveProfile& save_profile;
    SessionParticipantContext participants;
};

struct SessionGameContext {
    HostLaunchPlan& launch_plan;
    RetroArchLaunchConfig& launch_config;
    SessionContentInfo& content;
};

struct SessionVideoContext {
    const CapturePlan& capture;
    SessionCaptureDevices& devices;
    std::string_view video_resolution;
    int retroarch_scale = 1;
    int switch_scale = 1;
};

struct SessionInputContext {
    SessionDevicePlan& devices;
};

struct SessionBackendPrepareContext {
    const HostAppConfig& config;
    SessionUserContext user;
    SessionGameContext game;
    SessionVideoContext video;
    SessionInputContext input;
    SessionBackendState& backends;
    EmulatorLaunchEnvRequest& launch_env_request;
};

struct SessionRetroArchOverrideOptions {
    bool use_virtual_capture = false;
    int slot_index = 0;
    std::uint16_t network_cmd_port = 55355;
    DisplayLayoutPreference display_layout = DisplayLayoutPreference::Auto;
};

enum class SessionPadPlanKind {
    Direct,
    RetroArchSlot,
};

void apply_capture_to_session_device_plan(
    SessionDevicePlan& devices,
    const HostAppConfig& config,
    const CapturePlan& capture,
    std::optional<GpuDevice> resolved_gpu = std::nullopt);

SessionDevicePlan resolve_session_device_plan(
    const HostAppConfig& config,
    const HostLaunchPlan& launch_plan,
    SessionPadPlanKind kind,
    std::uint16_t product_id_base);

SessionMediaPlan build_session_media_plan(
    const HostAppConfig& config,
    SessionPlan* plan,
    SessionMediaPlanKind kind);

SessionContentInfo resolve_session_content_info(
    GameCatalog& catalog,
    const HostLaunchPlan& launch_plan);

SessionLaunchAssets prepare_session_launch_assets(
    HostAppConfig& config,
    GameCatalog& catalog,
    const HostLaunchPlan& launch_plan,
    std::string_view log_prefix = {});

SessionLaunchContext prepare_session_launch_context(
    HostAppConfig& config,
    GameCatalog& catalog,
    HostLaunchPlan launch_plan,
    std::string_view log_prefix = {});

void apply_session_content_launch_adjustments(
    HostAppConfig& config,
    RetroArchLaunchConfig& launch_config,
    const SaveProfile& save_profile,
    const ResolvedRetroArch& resolved_retroarch,
    const SessionContentInfo& content,
    std::string_view log_prefix = {});

void prepare_session_standalone_backend(
    std::string_view system_key,
    RetroArchLaunchConfig& launch_config,
    SessionBackendState& backends);

SwitchLaunchContent resolve_switch_launch_content(
    const SaveProfile& save_profile,
    const RetroArchLaunchConfig& launch_config,
    const SessionContentInfo& content);

SessionParticipantContext resolve_direct_session_participants(
    std::string_view save_username);

SessionParticipantContext resolve_session_plan_participants(
    std::string_view save_username,
    const SessionPlan& plan);

SessionBackendPrepareContext make_session_backend_prepare_context(
    const HostAppConfig& config,
    HostLaunchPlan& launch_plan,
    SessionLaunchAssets& assets,
    SessionDevicePlan& devices,
    SessionBackendState& backends,
    const CapturePlan& capture,
    EmulatorLaunchEnvRequest& launch_env_request,
    SessionParticipantContext participants);

RetroArchOverrideParams build_session_retroarch_override(
    const SessionBackendPrepareContext& backend,
    const SessionRetroArchOverrideOptions& options);

void plug_session_gamepads(VirtualGamepadBus& gamepads, RetroArchPort players);

void wait_for_session_input_enumeration();

SessionGpuSelection resolve_session_gpu_selection(
    const HostAppConfig& config,
    const CapturePlan& capture,
    std::string_view log_prefix = {},
    bool detailed_logging = false);

void apply_session_gpu_to_launch_request(
    EmulatorLaunchEnvRequest& request,
    const SessionGpuSelection& selection);

void append_session_controller_ignore_list(
    HostAppConfig& config,
    const std::optional<ControllerDevice>& bridge_device = std::nullopt,
    std::string_view log_prefix = {},
    bool warn_if_not_sdl2 = false);

} // namespace archstreamer
