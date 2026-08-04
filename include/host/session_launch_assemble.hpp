#pragma once

#include "common/protocol.hpp"
#include "host/capture_platform.hpp"
#include "host/host_app_config.hpp"
#include "host/launch_environment.hpp"
#include "host/retroarch_process.hpp"
#include "host/save_profile.hpp"
#include "host/switch/switch_backend.hpp"
#include "host/virtual_gamepad.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace archstreamer {

struct GpuDevice;

/**
 * Apply capture command prefix (standalone Gamescope/VGL or RetroArch VGL),
 * then compose and assign the launch environment on launch_config.
 * Callers still write RetroArch -c overrides themselves (or use apply_retroarch_override).
 */
void apply_capture_and_launch_environment(
    RetroArchLaunchConfig& launch_config,
    const CapturePlan& capture,
    const HostAppConfig& config,
    const std::string& gamescope_vk_device,
    const std::optional<GpuDevice>& resolved_gpu,
    const EmulatorLaunchEnvRequest& env_request);

/**
 * Sync Switch shared saves after emulator exit and log when any titles moved.
 * When backend is set, uses backend->post_exit_sync; otherwise syncs directly.
 * When slot_index is set, uses the lobby slot-prefixed log line.
 */
void sync_and_log_post_exit_switch_saves(
    const SaveProfile& profile,
    std::optional<int> slot_index = std::nullopt,
    const SwitchBackend* backend = nullptr,
    std::string_view content_stem = {},
    std::string_view title_id = {});

/** Replace an existing RetroArch -c path, or append -c <path> if missing. */
void rewrite_retroarch_config_arg(
    RetroArchLaunchConfig& launch_config,
    const std::filesystem::path& runtime_override);

/** Refresh launch_config.environment / unset_environment from a request. */
void apply_launch_env_to_config(
    RetroArchLaunchConfig& launch_config,
    const EmulatorLaunchEnvRequest& request);

/** Packed args for write_retroarch_input_override across launch / relaunch paths. */
struct RetroArchOverrideParams {
    std::size_t first_virtual_joypad_index = 0;
    const std::vector<VirtualGamepadIdentity>* identities = nullptr;
    std::string joypad_driver;
    RetroArchPort players = 1;
    const SaveProfile* save_profile = nullptr;
    bool realtime_pacing = false;
    bool capture_fullscreen = false;
    std::string capture_resolution;
    int vulkan_gpu_index = -1;
    std::string system_key;
    std::filesystem::path core_path;
    int resolution_scale = 1;
    int slot_index = 0;
    std::uint16_t network_cmd_port = 55355;
    DisplayLayoutPreference display_layout = DisplayLayoutPreference::Auto;
};

/** Prefer Portrait if any video client asks for it; else first explicit preference; else Auto. */
DisplayLayoutPreference resolve_display_layout_preference(
    const std::optional<ClientHello>& host_hello,
    const std::vector<ClientHello>& client_hellos);

/**
 * Write a RetroArch input override and set/replace the launch_config -c arg.
 * Returns the override path written.
 */
std::filesystem::path apply_retroarch_override(
    RetroArchLaunchConfig& launch_config,
    const RetroArchOverrideParams& params);

/**
 * Same as apply_retroarch_override, then refresh launch environment (relaunch paths).
 */
std::filesystem::path apply_retroarch_override_and_env(
    RetroArchLaunchConfig& launch_config,
    const RetroArchOverrideParams& params,
    const EmulatorLaunchEnvRequest& env_request);

} // namespace archstreamer
