#pragma once

#include "common/protocol.hpp"
#include "host/console/launch_content.hpp"
#include "host/contracts/launch.hpp"
#include "host/contracts/present.hpp"
#include "host/hardware/gpu_selection.hpp"
#include "host/host_app_config.hpp"
#include "host/host_launch_planner.hpp"
#include "host/virtual/device_plan.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace archstreamer {

struct SessionPlan;

struct SessionLaunchEnvironment {
    PresentTarget present;
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

enum class SessionMediaPlanKind {
    Direct,
    Slot,
};

struct SessionLaunchContext {
    HostLaunchPlan launch_plan;
    SessionLaunchAssets assets;
};

struct SessionParticipantContext {
    std::vector<ClientHello> client_hellos;
    std::string profile_display_name;
    DisplayLayoutPreference display_layout = DisplayLayoutPreference::Auto;
};

SessionMediaPlan build_session_media_plan(
    const HostAppConfig& config,
    SessionPlan* plan,
    SessionMediaPlanKind kind);

SessionLaunchContext prepare_session_launch_context(
    HostAppConfig& config,
    GameCatalog& catalog,
    HostLaunchPlan launch_plan,
    std::string_view log_prefix = {});

SessionParticipantContext resolve_direct_session_participants(
    std::string_view save_username);

SessionParticipantContext resolve_session_plan_participants(
    std::string_view save_username,
    const SessionPlan& plan);

/** Translate host video settings + emulator facts into a PresentTarget. */
PresentTarget resolve_session_present_target(
    HostAppConfig& config,
    const RetroArchLaunchConfig& launch_config);

/** Copy present/audio into request.plug (xtest_display is the keyboard/gamescope lease). */
void bind_session_plug(
    EmulatorLaunchEnvRequest& request,
    const PresentTarget& present,
    const HostAppConfig& config,
    std::string xtest_display);

} // namespace archstreamer
