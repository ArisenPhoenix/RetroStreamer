#pragma once

#include "common/protocol.hpp"
#include "host/cadence_session_tracker.hpp"
#include "host/game_catalog.hpp"
#include "host/host_app_config.hpp"
#include "host/host_launch_planner.hpp"
#include "host/media_server.hpp"
#include "host/nds/melonds_ctrl_client.hpp"
#include "host/retroarch_process.hpp"
#include "host/retroarch_resolve.hpp"
#include "host/save_profile.hpp"
#include "host/session_launch_types.hpp"
#include "host/session_runtime.hpp"
#include "host/virtual_keyboard.hpp"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace archstreamer {

class InputRouter;
struct ControllerDevice;

struct SessionLaunchTarget {
    HostLaunchPlan launch_plan;
    SaveProfile save_profile;
    RetroArchLaunchConfig launch_config;
    ResolvedRetroArch resolved_retroarch;
    std::string system_key;
    std::filesystem::path catalog_content_path;
    std::string m3m_title_id;
};

void append_direct_controller_ignore_list(
    HostAppConfig& config,
    const std::optional<ControllerDevice>& bridge_device);

std::optional<SessionLaunchTarget> prepare_direct_session_target(
    HostAppConfig& config,
    GameCatalog& catalog,
    const GameList& list,
    const std::optional<HostPlayerControllerIdentity>& bridge_identity);

void print_direct_launch_summary(
    const HostAppConfig& config,
    const HostLaunchPlan& launch_plan,
    const SaveProfile& save_profile,
    const RetroArchLaunchConfig& launch_config,
    const std::vector<MediaClientStream>& media_streams,
    const std::string& capture_display);

void log_direct_emulator_command(
    const RetroArchLaunchConfig& launch_config,
    const ResolvedRetroArch& resolved_retroarch);

std::unique_ptr<MelonDsCtrlClient> configure_direct_input_router(
    InputRouter& input_router,
    VirtualKeyboard& keyboard,
    const HostLaunchPlan& launch_plan,
    const std::unique_ptr<SwitchBackend>& switch_backend,
    const std::unique_ptr<MelonDsBackend>& melonds_backend);

void print_input_seats(const SeatAssignment& seats);

CadenceSessionTracker begin_direct_cadence_session(
    const HostLaunchPlan& launch_plan,
    const HostAppConfig& config,
    const SessionRuntime& session_runtime);

void end_direct_cadence_session(
    CadenceSessionTracker& cadence_tracker,
    const HostLaunchPlan& launch_plan,
    std::string_view end_reason);

SessionLaunchEnvironment prepare_direct_launch_environment(
    HostAppConfig& config,
    RetroArchLaunchConfig& launch_config,
    bool host_plays_locally);

SessionMediaPlan build_direct_media_plan(const HostAppConfig& config);

SessionDevicePlan resolve_direct_device_plan(
    const HostLaunchPlan& launch_plan,
    const HostAppConfig& config,
    const CapturePlan& capture);

void prepare_direct_backend(
    const HostAppConfig& config,
    RetroArchLaunchConfig& launch_config,
    const CapturePlan& capture,
    const SessionLaunchTarget& target,
    HostLaunchPlan& launch_plan,
    SessionDevicePlan& devices,
    VirtualKeyboard& keyboard,
    EmulatorLaunchEnvRequest& launch_env_request,
    SessionBackendState& backends);

} // namespace archstreamer
