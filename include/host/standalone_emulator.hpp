#pragma once

#include "host/save_profile.hpp"

#include <filesystem>
#include <optional>
#include <string>
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

// Managed tree: ~/.local/share/archstreamer/ryujinx/{Ryujinx,keys/...}
// Discovery only for now — Switch sessions still launch via Yuzu.
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

} // namespace archstreamer
