#pragma once

#include "host/console/nds/melonds_backend.hpp"
#include "host/console/retroarch_config_writer.hpp"
#include "host/console/switch/switch_backend.hpp"
#include "host/session/launch_types.hpp"
#include "host/video/capture_platform.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace archstreamer {

struct SessionBackendState {
    std::unique_ptr<SwitchBackend> switch_backend;
    std::unique_ptr<MelonDsBackend> melonds_backend;
    std::string switch_launch_content_stem;
    std::string switch_launch_title_id;
    bool switch_launch_uses_m3m_map = false;
    std::string system_key;
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
    int slot_index = 0;
    std::uint16_t network_cmd_port = 55355;
    DisplayLayoutPreference display_layout = DisplayLayoutPreference::Auto;
};

void prepare_session_standalone_backend(
    std::string_view system_key,
    RetroArchLaunchConfig& launch_config,
    SessionBackendState& backends);

SessionBackendPrepareContext make_session_backend_prepare_context(
    const HostAppConfig& config,
    HostLaunchPlan& launch_plan,
    SessionLaunchAssets& assets,
    SessionDevicePlan& devices,
    SessionCaptureDevices& capture,
    SessionBackendState& backends,
    EmulatorLaunchEnvRequest& launch_env_request,
    SessionParticipantContext participants);

RetroArchOverrideParams build_session_retroarch_override(
    const SessionBackendPrepareContext& backend,
    const SessionRetroArchOverrideOptions& options);

} // namespace archstreamer
