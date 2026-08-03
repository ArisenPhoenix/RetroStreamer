#include "host/nds/melonds_user_profile.hpp"

#include "host/nds_display_layout.hpp"
#include "host/platform/host_pad_platform.hpp"
#include "host/standalone_emulator.hpp"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace archstreamer {
namespace {

std::string sanitize_player_name(std::string_view preferred) {
    std::string name;
    name.reserve(std::min<std::size_t>(preferred.size(), 10));
    for (char character : preferred) {
        if (name.size() >= 10) {
            break;
        }
        const auto code = static_cast<unsigned char>(character);
        if (code < 0x20 || code == 0x7f) {
            continue;
        }
        name.push_back(character);
    }
    while (!name.empty() && (name.back() == ' ' || name.back() == '\t')) {
        name.pop_back();
    }
    if (name.empty()) {
        return "Player";
    }
    return name;
}

std::string mac_for_username(std::string_view username) {
    std::uint64_t hash = 1469598103934665603ull;
    for (unsigned char c : username) {
        hash ^= c;
        hash *= 1099511628211ull;
    }
    std::ostringstream out;
    out << "02";
    for (int i = 0; i < 5; ++i) {
        const auto byte = static_cast<unsigned>((hash >> (8 * i)) & 0xffu);
        out << ':' << std::hex << std::setw(2) << std::setfill('0') << byte;
    }
    return out.str();
}

void write_toml_string(std::ostream& out, std::string_view key, std::string_view value) {
    out << key << " = \"";
    for (char c : value) {
        if (c == '\\' || c == '"') {
            out << '\\';
        }
        out << c;
    }
    out << "\"\n";
}

void write_joystick_table(std::ostream& out, bool portrait_layout) {
    // Raw SDL_JoystickGetButton indices for ArchStreamer uinput (EV_KEY ascending):
    // 0 SOUTH/A, 1 EAST/B, 2 NORTH/Y, 3 WEST/X, 4 TL, 5 TR, 6 Select, 7 Start,
    // 8 Guide, 9–10 sticks, 11–14 D-pad. melonDS does not use GameController button IDs.
    //
    // R2 = axis 5 trigger (same encoding as before). Landscape EmphTop uses
    // HK_SwapScreenEmphasis (large top ↔ large bottom); portrait Even uses
    // HK_SwapScreens — matches libretro melonds_swapscreen_mode=Toggle / R2.
    constexpr int kJoyR2Trigger = (5 << 24) | (2 << 20) | 0x10000 | 0xFFFF;
    out << "\n[Instance0.Joystick]\n";
    out << "A = 0\n";
    out << "B = 1\n";
    out << "Select = 6\n";
    out << "Start = 7\n";
    out << "Right = 14\n";
    out << "Left = 13\n";
    out << "Up = 11\n";
    out << "Down = 12\n";
    out << "R = 5\n";
    out << "L = 4\n";
    out << "X = 3\n";
    out << "Y = 2\n";
    if (portrait_layout) {
        out << "HK_SwapScreens = " << kJoyR2Trigger << "\n";
        out << "HK_SwapScreenEmphasis = -1\n";
    } else {
        out << "HK_SwapScreens = -1\n";
        out << "HK_SwapScreenEmphasis = " << kJoyR2Trigger << "\n";
    }
    out << "HK_Lid = -1\n";
    out << "HK_Mic = -1\n";
    out << "HK_Pause = -1\n";
    out << "HK_Reset = -1\n";
    out << "HK_FastForward = -1\n";
    out << "HK_FastForwardToggle = -1\n";
    out << "HK_FrameLimitToggle = -1\n";
    out << "HK_FullscreenToggle = -1\n";
    out << "HK_FrameStep = -1\n";
    out << "HK_SlowMo = -1\n";
    out << "HK_SlowMoToggle = -1\n";
    out << "HK_PowerButton = -1\n";
    out << "HK_VolumeUp = -1\n";
    out << "HK_VolumeDown = -1\n";
    out << "HK_SolarSensorIncrease = -1\n";
    out << "HK_SolarSensorDecrease = -1\n";
}

} // namespace

std::filesystem::path melonds_system_bios_root() {
    return default_archstreamer_data_root() / "system" / "nds";
}

void apply_melonds_pad_seed(
    MelonDsProfileSeed& seed,
    const std::vector<ArchStreamerSdlPad>& pads,
    const std::string& sdl_device_filter) {
    seed.sdl_device_filter = sdl_device_filter;
    seed.sdl_gamecontroller_config.clear();
    if (pads.empty()) {
        std::cerr << "Warning: no ArchStreamer SDL pads; melonDS JoystickID left at "
                  << seed.joystick_id << '\n';
        return;
    }
    seed.joystick_id = static_cast<int>(pads.front().sdl_index);
    std::ostringstream mapping;
    // Same mapping string as Ryujinx — keeps GUID+platform matching under gamescope.
    mapping << pads.front().mapping_guid << ",ArchStreamer,"
            << "platform:" << kSdlGameControllerPlatform << ","
            << "a:b0,b:b1,y:b2,x:b3,"
            << "leftshoulder:b4,rightshoulder:b5,back:b6,start:b7,"
            << "guide:b8,leftstick:b9,rightstick:b10,"
            << "dpup:b11,dpdown:b12,dpleft:b13,dpright:b14,"
            << "leftx:a0,lefty:a1,rightx:a3,righty:a4,lefttrigger:a2,righttrigger:a5,";
    seed.sdl_gamecontroller_config = mapping.str();
}

MelonDsUserProfile prepare_melonds_user_profile(
    const SaveProfile& save_profile,
    std::string_view profile_display_name,
    int slot_index,
    const MelonDsProfileSeed& seed) {
    MelonDsUserProfile profile;
    profile.xdg_config_home = save_profile.user_directory / "melonds" / "xdg-config";
    profile.config_dir = profile.xdg_config_home / "melonDS";
    profile.config_path = profile.config_dir / "melonDS.toml";
    profile.save_directory = save_profile.user_directory / "melonds" / "saves";
    profile.player_name = sanitize_player_name(profile_display_name);
    profile.ctrl_server_name = melonds_ctrl_server_name_for_slot(slot_index);
    profile.sdl_device_filter = seed.sdl_device_filter;
    profile.sdl_gamecontroller_config = seed.sdl_gamecontroller_config;

    std::filesystem::create_directories(profile.config_dir);
    std::filesystem::create_directories(profile.save_directory);

    const auto bios_root = melonds_system_bios_root();
    const auto layout = resolve_nds_display_layout(seed.display_layout);

    std::ofstream out(profile.config_path, std::ios::trunc);
    if (!out) {
        throw std::runtime_error("failed to write melonDS config: " + profile.config_path.string());
    }

    // One table per path — duplicate [Instance0] tables make toml11 throw and melonDS
    // falls back to unbound defaults (all Joystick = -1).
    out << "PauseLostFocus = false\n";
    out << "AudioSync = false\n";
    out << "TargetFPS = 60.0\n";
    out << "LimitFPS = true\n";
    write_toml_string(out, "LastBIOSFolder", bios_root.string());

    out << "\n[Audio]\n";
    out << "Interpolation = 0\n";
    out << "BitDepth = 0\n";

    out << "\n[Emu]\n";
    out << "DirectBoot = true\n";
    out << "ExternalBIOSEnable = true\n";
    out << "ConsoleType = 0\n";

    out << "\n[LAN]\n";
    out << "DirectMode = false\n";
    out << "HostNumPlayers = 2\n";
    write_toml_string(out, "PlayerName", profile.player_name);
    write_toml_string(out, "Device", "");

    out << "\n[MP]\n";
    out << "RecvTimeout = 25\n";
    out << "AudioMode = 1\n";

    out << "\n[DS]\n";
    write_toml_string(out, "BIOS7Path", (bios_root / "bios7.bin").string());
    write_toml_string(out, "BIOS9Path", (bios_root / "bios9.bin").string());
    write_toml_string(out, "FirmwarePath", (bios_root / "firmware.bin").string());

    out << "\n[Screen]\n";
    out << "VSync = false\n";
    out << "UseGL = true\n";

    out << "\n[3D]\n";
    out << "Renderer = 1\n"; // OpenGL — matches RetroArch melonDS streaming path

    out << "\n[Instance0]\n";
    write_toml_string(out, "SaveFilePath", profile.save_directory.string());
    out << "JoystickID = " << seed.joystick_id << "\n";

    out << "\n[Instance0.Audio]\n";
    out << "Volume = 256\n";
    out << "DSiVolumeSync = false\n";

    out << "\n[Instance0.Firmware]\n";
    out << "OverrideSettings = true\n";
    write_toml_string(out, "Username", profile.player_name);
    write_toml_string(out, "MAC", mac_for_username(save_profile.username));
    out << "Language = 1\n";
    out << "BirthdayMonth = 1\n";
    out << "BirthdayDay = 1\n";
    out << "FavouriteColour = 0\n";
    write_toml_string(out, "Message", "");

    // Hybrid Top policy: Horizontal + EmphTop (large top, small bottom). Portrait stacks
    // equal screens — same outcomes as apply_nds_screen_layout() for the libretro core.
    out << "\n[Instance0.Window0]\n";
    out << "ScreenLayout = " << layout.window_screen_layout << "\n";
    out << "ScreenSizing = " << layout.window_screen_sizing << "\n";
    out << "ScreenGap = 0\n";
    out << "ScreenSwap = false\n";
    out << "ScreenRotation = 0\n";
    out << "IntegerScaling = false\n";
    out << "ScreenFilter = false\n";
    out << "ShowOSD = false\n";

    write_joystick_table(out, layout.portrait);

    std::cout << "melonDS config: JoystickID=" << seed.joystick_id
              << " layout=" << layout.core_layout
              << " filter=" << (seed.sdl_device_filter.empty() ? "(none)" : seed.sdl_device_filter)
              << '\n';
    return profile;
}

std::vector<std::pair<std::string, std::string>> melonds_launch_environment(
    const MelonDsUserProfile& profile) {
    std::vector<std::pair<std::string, std::string>> env{
        {"XDG_CONFIG_HOME", profile.xdg_config_home.string()},
        {"SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", "1"},
        // Prefer Pulse so PULSE_SINK=archstreamer-N from audio_launch_environment applies.
        {"SDL_AUDIODRIVER", "pulse"},
    };
    if (!profile.sdl_gamecontroller_config.empty()) {
        env.emplace_back("SDL_GAMECONTROLLERCONFIG", profile.sdl_gamecontroller_config);
    }
    if (!profile.sdl_device_filter.empty()) {
        env.emplace_back("SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT", profile.sdl_device_filter);
    }
    return env;
}

} // namespace archstreamer
