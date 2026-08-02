#pragma once

#include "common/protocol.hpp"
#include "host/gpu_select.hpp"
#include "host/media_capture.hpp"
#include "host/retroarch_process.hpp"
#include "host/save_profile.hpp"
#include "host/virtual_joypad_resolve.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace archstreamer {

struct ResolvedStandaloneEmulator {
    std::filesystem::path path;
    // Args inserted before the content path (e.g. Yuzu: -f -g).
    std::vector<std::string> args_before_content;
    std::string display_name;
};

struct YuzuUserProfile {
    // Per-session XDG roots so each ArchStreamer user gets isolated Yuzu saves/config.
    std::filesystem::path xdg_data_home;
    std::filesystem::path xdg_config_home;
    std::filesystem::path keys_directory;
};

struct RyujinxUserProfile {
    // XDG_CONFIG_HOME → <user>/ryujinx/xdg-config/Ryujinx/{Config.json,bis,system,...}
    std::filesystem::path xdg_config_home;
    std::filesystem::path data_root; // .../xdg-config/Ryujinx
    std::filesystem::path keys_directory;
    // Injected into the Ryujinx child so ArchStreamer uinput pads show up as SDL GameControllers.
    std::string sdl_gamecontroller_config;
    // Hides every joystick except this session's pads, so the SDL indices baked into
    // Config.json stay valid no matter what else is plugged in or which other sessions run.
    std::string sdl_device_filter;
};

// ArchStreamer-owned shared data (not personal ~/.config/Ryujinx or ~/.local/share/yuzu).
// Layout: <data>/system/<device>/{keys,firmware/...}
// Example: ~/.local/share/archstreamer/system/switch/{keys,firmware/registered}
std::filesystem::path default_archstreamer_data_root();
std::filesystem::path switch_system_defaults_root();

// Managed tree: ~/.local/share/archstreamer/yuzu/{yuzu.AppImage,keys/...}
std::filesystem::path default_yuzu_runtime_root();

// True when a managed binary exists or a discoverable source install is present.
// Does not copy/install; safe for catalog scans.
bool yuzu_runtime_available();

// Human-readable reason for clients/host logs when Switch cannot be offered.
std::string yuzu_unavailable_message();

// Ensure AppImage/exe + shared keys exist under the managed runtime (copy from
// a discovered source when missing). Returns nullopt if no binary can be found.
std::optional<ResolvedStandaloneEmulator> ensure_yuzu_runtime();

// Catalog/launch resolve: only succeeds when a Yuzu binary is available.
// Catalog callers should prefer yuzu_runtime_available() first to avoid installs
// during listing; this still no-ops when nothing is discoverable.
inline std::optional<ResolvedStandaloneEmulator> resolve_yuzu() {
    if (!yuzu_runtime_available()) {
        return std::nullopt;
    }
    return ensure_yuzu_runtime();
}

// Create per-user Yuzu data/config under the save profile and seed keys.
// force_opengl: VirtualGL/Xvfb path (Vulkan often lacks a present queue there).
// force_vulkan: gamescope path (OpenGL left over from VGL sessions presents poorly).
// vulkan_device: Yuzu qt-config vulkan_device index (-1 = leave unchanged).
// resolution_scale: internal render multiplier 1–6 (1x…6x native); 0 or negative = leave unchanged.
YuzuUserProfile prepare_yuzu_user_profile(
    const SaveProfile& save_profile,
    bool force_opengl = false,
    bool force_vulkan = false,
    int vulkan_device = -1,
    int resolution_scale = 1);

// Bind player_N Controls to ArchStreamer uinput pads (engine:sdl + GUID).
// Call after pads are plugged so GUIDs match what Yuzu's SDL will see.
void configure_yuzu_archstreamer_controls(
    const YuzuUserProfile& profile,
    const std::vector<std::string>& sdl_guids);

std::vector<std::pair<std::string, std::string>> yuzu_launch_environment(
    const YuzuUserProfile& profile);

// Managed tree: ~/.local/share/archstreamer/ryujinx/{Ryujinx.AppImage,keys/...}
// Preferred Switch runtime when available; Yuzu remains the fallback.
std::filesystem::path default_ryujinx_runtime_root();
bool ryujinx_runtime_available();
std::string ryujinx_unavailable_message();
std::optional<ResolvedStandaloneEmulator> ensure_ryujinx_runtime();

inline std::optional<ResolvedStandaloneEmulator> resolve_ryujinx() {
    if (!ryujinx_runtime_available()) {
        return std::nullopt;
    }
    return ensure_ryujinx_runtime();
}

/**
 * Prefer Ryujinx for Switch when installed; otherwise Yuzu.
 * enable_ldn_mitm: write multiplayer_mode=ldn_mitm into the user Config.json.
 */
std::optional<ResolvedStandaloneEmulator> resolve_switch_runtime();
std::string switch_runtime_unavailable_message();

RyujinxUserProfile prepare_ryujinx_user_profile(
    const SaveProfile& save_profile,
    bool enable_ldn_mitm = true,
    int resolution_scale = 1,
    std::string_view profile_display_name = {});

// Bind Player1…N to ArchStreamer uinput pads (GamepadSDL2 + GUID) and build an
// SDL_GAMECONTROLLERCONFIG mapping so Ryujinx can open those pads under gamescope.
// pads must carry the SDL joystick indices the Ryujinx child will see; pass the
// sdl_device_filter that guarantees it (see resolve_exclusive_archstreamer_pads).
void configure_ryujinx_archstreamer_controls(
    RyujinxUserProfile& profile,
    const std::vector<ArchStreamerSdlPad>& pads,
    const std::string& sdl_device_filter = {});

std::vector<std::pair<std::string, std::string>> ryujinx_launch_environment(
    const RyujinxUserProfile& profile);

/**
 * Lobby display-name policy: prefer a matching ClientHello.display_name for the
 * save username (host hello first, then seated clients). Falls back to username.
 */
std::string resolve_switch_profile_display_name(
    std::string_view save_username,
    const std::optional<ClientHello>& host_hello,
    const std::vector<ClientHello>& client_hellos);

/** Inputs for shared Ryujinx/Yuzu standalone session preparation. */
struct SwitchStandalonePrepInput {
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
};

struct SwitchStandalonePrepResult {
    bool use_ryujinx = false;
    /** Backend-optional pad OSK; Ryujinx sets true, Yuzu leaves false. */
    bool enable_soft_keyboard = false;
    bool force_opengl = false;
    bool force_vulkan = false;
    int yuzu_vulkan_device = -1;
    std::size_t synced_title_count = 0;
    std::optional<RyujinxUserProfile> ryujinx_profile;
    std::optional<YuzuUserProfile> yuzu_profile;
    std::vector<ArchStreamerSdlPad> resolved_pads;
};

/**
 * Shared Switch standalone prep used by direct host and lobby session slots.
 * Assumes launch_config.core_path is already set by resolve_switch_runtime.
 * Sets standalone_args_before_content and quiet_stdio; runs save sync.
 */
SwitchStandalonePrepResult prepare_switch_standalone(
    RetroArchLaunchConfig& launch_config,
    SwitchStandalonePrepInput input);

} // namespace archstreamer
