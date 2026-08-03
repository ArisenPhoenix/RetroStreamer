#pragma once

#include "common/protocol.hpp"
#include "host/save_profile.hpp"
#include "host/virtual_joypad_resolve.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace archstreamer {

/**
 * Per-user melonDS isolation (mirrors Ryujinx XDG style).
 * XDG_CONFIG_HOME → <user>/melonds/xdg-config → melonDS writes melonDS/melonDS.toml there.
 */
struct MelonDsUserProfile {
    std::filesystem::path xdg_config_home;
    std::filesystem::path config_dir;   // .../melonDS
    std::filesystem::path config_path;  // .../melonDS.toml
    /** SaveFilePath — shared with RetroArch: <user>/saves */
    std::filesystem::path save_directory;
    /** QLocalServer name passed as --archstreamer-ctrl (socket under /tmp on Linux). */
    std::string ctrl_server_name;
    std::string player_name;
    /** Child SDL filter so JoystickID indices stay stable (Ryujinx-style). */
    std::string sdl_device_filter;
    std::string sdl_gamecontroller_config;
};

struct MelonDsProfileSeed {
    int joystick_id = 0;
    DisplayLayoutPreference display_layout = DisplayLayoutPreference::Auto;
    std::string sdl_device_filter;
    std::string sdl_gamecontroller_config;
};

/**
 * Write a single valid melonDS.toml (no duplicate tables — melonDS aborts parse
 * and falls back to unbound defaults if TOML is invalid).
 */
MelonDsUserProfile prepare_melonds_user_profile(
    const SaveProfile& save_profile,
    std::string_view profile_display_name,
    int slot_index,
    const MelonDsProfileSeed& seed = {});

/** Fill seed.sdl_* from exclusive ArchStreamer pads (JoystickID + GAMECONTROLLERCONFIG). */
void apply_melonds_pad_seed(
    MelonDsProfileSeed& seed,
    const std::vector<ArchStreamerSdlPad>& pads,
    const std::string& sdl_device_filter);

std::vector<std::pair<std::string, std::string>> melonds_launch_environment(
    const MelonDsUserProfile& profile);

/** Shared BIOS/firmware tree: ~/.local/share/archstreamer/system/nds */
std::filesystem::path melonds_system_bios_root();

/** Stable QLocalServer name for a session slot (must match MelonDsBackend::prepare). */
inline std::string melonds_ctrl_server_name_for_slot(int slot_index) {
    return "archstreamer-melonds-" + std::to_string(slot_index < 0 ? 0 : slot_index);
}

} // namespace archstreamer
