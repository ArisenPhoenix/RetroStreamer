#pragma once

#include "host/gpu_select.hpp"
#include "host/launch_environment.hpp"
#include "host/media_capture.hpp"
#include "host/pad_plan.hpp"
#include "host/retroarch_process.hpp"
#include "host/save_profile.hpp"
#include "host/standalone_emulator.hpp"
#include "host/virtual_joypad_resolve.hpp"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace archstreamer {

/** Inputs for Switch standalone session preparation (Ryujinx or Yuzu). */
struct SwitchBackendPrepContext {
    const SaveProfile& save_profile;
    std::size_t players = 1;
    bool verbose = false;
    std::uint16_t product_id_base = 0;
    std::string ignore_controller;
    GraphicsApiPreference graphics_api = GraphicsApiPreference::Auto;
    bool virtualgl_capture = false;
    bool gamescope_capture = false;
    int resolution_scale = 1;
    const std::optional<GpuDevice>* resolved_gpu = nullptr;
    /** Caller-resolved Ryujinx profile name (Steam persona or session hello). */
    std::string profile_display_name;
    std::vector<ArchStreamerSdlPad> resolved_pads;
    /** Concurrent session slot (0/1/…) for LDN netns IP assignment. */
    std::size_t slot_index = 0;
    /** Catalog ROM stem (e.g. "Pokemon Shield 1.3.2") — keys save + addon dirs. */
    std::string content_stem;
    /** Nintendo application title id when known (0100…). */
    std::string title_id;
};

struct SwitchBackendPrepResult {
    bool force_opengl = false;
    bool force_vulkan = false;
    int yuzu_vulkan_device = -1;
    std::size_t synced_title_count = 0;
    std::optional<RyujinxUserProfile> ryujinx_profile;
    std::optional<YuzuUserProfile> yuzu_profile;
    std::vector<ArchStreamerSdlPad> resolved_pads;
    std::optional<PadPlan> pad_plan;
};

/**
 * Virtual Switch emulator backend (Ryujinx / Yuzu). Owns prepare, env-profile
 * assignment, soft-keyboard capability, and post-exit save sync.
 */
class SwitchBackend {
public:
    virtual ~SwitchBackend() = default;

    virtual const char* name() const = 0;

    /**
     * Prepare profiles/controls/args for launch. Assumes launch_config.core_path
     * is already set by resolve_switch_runtime. Sets standalone_args_before_content
     * and quiet_stdio; runs pre-launch save sync.
     */
    virtual SwitchBackendPrepResult prepare(
        RetroArchLaunchConfig& launch_config,
        SwitchBackendPrepContext ctx) = 0;

    /** Assign ryujinx_profile or yuzu_profile onto the launch env request. */
    virtual void assign_launch_env_profile(
        EmulatorLaunchEnvRequest& env,
        SwitchBackendPrepResult& prep) const = 0;

    /** Backend-optional pad OSK; Ryujinx returns true. */
    virtual bool enable_soft_keyboard() const { return false; }

    /**
     * Pull in-session Switch saves into the catalog stem leaf after exit.
     * Uses content_stem/title_id from the last prepare when available.
     */
    virtual std::vector<std::string> post_exit_sync(
        const SaveProfile& profile,
        std::string_view content_stem = {},
        std::string_view title_id = {}) const;

protected:
    /** Shared pad discovery, graphics-API forcing, and quiet_stdio. */
    void apply_common_prep(
        RetroArchLaunchConfig& launch_config,
        SwitchBackendPrepContext& ctx,
        SwitchBackendPrepResult& result) const;

    void finish_prep_save_sync(
        const SwitchBackendPrepContext& ctx,
        SwitchBackendPrepResult& result) const;
};

class RyujinxBackend final : public SwitchBackend {
public:
    const char* name() const override { return "Ryujinx"; }
    SwitchBackendPrepResult prepare(
        RetroArchLaunchConfig& launch_config,
        SwitchBackendPrepContext ctx) override;
    void assign_launch_env_profile(
        EmulatorLaunchEnvRequest& env,
        SwitchBackendPrepResult& prep) const override;
    bool enable_soft_keyboard() const override { return true; }
};

class YuzuBackend final : public SwitchBackend {
public:
    const char* name() const override { return "Yuzu"; }
    SwitchBackendPrepResult prepare(
        RetroArchLaunchConfig& launch_config,
        SwitchBackendPrepContext ctx) override;
    void assign_launch_env_profile(
        EmulatorLaunchEnvRequest& env,
        SwitchBackendPrepResult& prep) const override;
};

/** Prefer runtime.display_name; falls back to path filename. */
std::unique_ptr<SwitchBackend> make_switch_backend(
    const ResolvedStandaloneEmulator& runtime);

/**
 * Log prep outcome after assign_launch_env_profile.
 * Direct path: detailed Ryujinx/Yuzu lines. Lobby: slot-prefixed compact lines.
 */
void log_switch_backend_prep(
    const SwitchBackend& backend,
    const EmulatorLaunchEnvRequest& env,
    const SwitchBackendPrepResult& prep,
    int resolution_scale,
    const std::optional<GpuDevice>& resolved_gpu = std::nullopt,
    std::optional<int> slot_index = std::nullopt);

} // namespace archstreamer
