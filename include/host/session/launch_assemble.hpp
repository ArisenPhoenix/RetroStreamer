#pragma once

#include "common/protocol.hpp"
#include "host/console/retroarch_config_writer.hpp"
#include "host/console/retroarch_process.hpp"
#include "host/console/save_profile.hpp"
#include "host/contracts/gpu.hpp"
#include "host/contracts/launch.hpp"
#include "host/contracts/present.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace archstreamer {

class SwitchBackend;

/**
 * Apply capture command prefix (standalone Gamescope/VGL or RetroArch VGL),
 * then compose and assign the launch environment on launch_config.
 * Callers still write RetroArch -c overrides themselves (or use apply_retroarch_override).
 */
void apply_capture_and_launch_environment(
    RetroArchLaunchConfig& launch_config,
    const PresentTarget& present,
    const std::string& video_resolution,
    const std::string& gamescope_vk_device,
    const std::optional<GpuTarget>& resolved_gpu,
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
    std::string_view title_id = {},
    bool uses_m3m_map = false);

/** Replace an existing RetroArch -c path, or append -c <path> if missing. */
void rewrite_retroarch_config_arg(
    RetroArchLaunchConfig& launch_config,
    const std::filesystem::path& runtime_override);

/** Refresh launch_config.environment / unset_environment from a request. */
void apply_launch_env_to_config(
    RetroArchLaunchConfig& launch_config,
    const EmulatorLaunchEnvRequest& request);

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
