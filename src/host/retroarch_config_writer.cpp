#include "host/retroarch_config_writer.hpp"

#include "common/platform/paths.hpp"
#include "host/retroarch_netcmd.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace archstreamer {

namespace {

std::filesystem::path retroarch_runtime_root() {
    if (const auto home = user_home_directory(); !home.empty()) {
        return std::filesystem::path(home) / ".local/share/archstreamer/retroarch";
    }
    return std::filesystem::current_path() / "archstreamer-retroarch";
}

std::string sanitize_device_name(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (const char character : value) {
        if (character == '"' || character == '\n' || character == '\r' || character == '\t') {
            result.push_back(' ');
        } else {
            result.push_back(character);
        }
    }
    return result;
}

// RetroPad uses SNES-oriented names (B=south, A=east, Y=west, X=north).
// uinput emits SDL-order indices: 0=SOUTH, 1=EAST, 2=WEST, 3=NORTH.
//
// Nintendo / default: map by Xbox/SDL *letter* (A→A) so a physical A presses
// the face button games label "A".
// PlayStation: map by *position* so DualShock Cross (south) hits RetroPad B →
// PS Cross, not Circle.
struct FaceButtonIndices {
    const char* b = "1";
    const char* a = "0";
    const char* y = "3";
    const char* x = "2";
};

FaceButtonIndices face_button_indices_for_system(std::string_view system_key) {
    if (system_key == "ps1" || system_key == "ps2" || system_key == "psp") {
        return FaceButtonIndices{"0", "1", "2", "3"};
    }
    return FaceButtonIndices{};
}

void write_virtual_pad_autoconfig(
    const std::filesystem::path& autoconfig_root,
    const VirtualGamepadIdentity& identity,
    const std::string& joypad_driver,
    RetroArchPort port,
    const FaceButtonIndices& face) {
    const auto directory = autoconfig_root / joypad_driver;
    std::filesystem::create_directories(directory);

    auto port_identity = identity;
    port_identity.name += " P" + std::to_string(static_cast<int>(port) + 1);
    port_identity.product_id = static_cast<std::uint16_t>(port_identity.product_id + port);

    const auto device_name = sanitize_device_name(port_identity.name);
    const auto path = directory / (device_name + ".cfg");
    std::ofstream file(path, std::ios::trunc);
    if (!file) {
        throw std::runtime_error("failed to write RetroArch virtual pad autoconfig");
    }

    file
        << "input_driver = \"" << joypad_driver << "\"\n"
        << "input_device = \"" << device_name << "\"\n"
        << "input_vendor_id = \"" << port_identity.vendor_id << "\"\n"
        << "input_product_id = \"" << port_identity.product_id << "\"\n"
        << "input_b_btn = \"" << face.b << "\"\n"
        << "input_a_btn = \"" << face.a << "\"\n"
        << "input_y_btn = \"" << face.y << "\"\n"
        << "input_x_btn = \"" << face.x << "\"\n"
        << "input_l_btn = \"4\"\n"
        << "input_r_btn = \"5\"\n"
        << "input_select_btn = \"6\"\n"
        << "input_start_btn = \"7\"\n"
        << "input_l3_btn = \"9\"\n"
        << "input_r3_btn = \"10\"\n"
        << "input_l2_axis = \"+2\"\n"
        << "input_r2_axis = \"+5\"\n"
        << "input_l_x_minus_axis = \"-0\"\n"
        << "input_l_x_plus_axis = \"+0\"\n"
        << "input_l_y_minus_axis = \"-1\"\n"
        << "input_l_y_plus_axis = \"+1\"\n"
        << "input_r_x_minus_axis = \"-3\"\n"
        << "input_r_x_plus_axis = \"+3\"\n"
        << "input_r_y_minus_axis = \"-4\"\n"
        << "input_r_y_plus_axis = \"+4\"\n"
        << "input_up_btn = \"11\"\n"
        << "input_down_btn = \"12\"\n"
        << "input_left_btn = \"13\"\n"
        << "input_right_btn = \"14\"\n";
}

void upsert_core_opt_file(
    const std::filesystem::path& path,
    const std::vector<std::pair<std::string, std::string>>& options) {
    if (options.empty()) {
        return;
    }
    std::filesystem::create_directories(path.parent_path());

    std::unordered_map<std::string, std::string> values;
    std::vector<std::string> order;
    if (std::ifstream in(path); in) {
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty() || line[0] == '#') {
                continue;
            }
            const auto eq = line.find('=');
            if (eq == std::string::npos) {
                continue;
            }
            auto key = line.substr(0, eq);
            while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) {
                key.pop_back();
            }
            if (values.emplace(key, line).second) {
                order.push_back(key);
            } else {
                values[key] = line;
            }
        }
    }

    for (const auto& [key, value] : options) {
        const auto line = key + " = \"" + value + "\"";
        if (values.emplace(key, line).second) {
            order.push_back(key);
        } else {
            values[key] = line;
        }
    }

    std::ofstream out(path, std::ios::trunc);
    if (!out) {
        return;
    }
    for (const auto& key : order) {
        out << values[key] << '\n';
    }
}

std::filesystem::path retroarch_core_opt_path(std::string_view core_dir_name) {
    const auto home = user_home_directory();
    if (home.empty()) {
        return {};
    }
    const auto dir = std::filesystem::path(home) / ".config/retroarch/config" / std::string(core_dir_name);
    return dir / (std::string(core_dir_name) + ".opt");
}

std::string core_file_key(const std::filesystem::path& core_path) {
    auto stem = core_path.stem().string();
    // libpcsx2_libretro / pcsx2_libretro / pcsx2.libretro → pcsx2
    if (stem.rfind("lib", 0) == 0) {
        stem = stem.substr(3);
    }
    constexpr std::string_view suffix = "_libretro";
    if (stem.size() > suffix.size() &&
        stem.compare(stem.size() - suffix.size(), suffix.size(), suffix) == 0) {
        stem.resize(stem.size() - suffix.size());
    }
    // Some distros use "name.libretro" as the stem when the file is name.libretro.so
    constexpr std::string_view dotted = ".libretro";
    if (stem.size() > dotted.size() &&
        stem.compare(stem.size() - dotted.size(), dotted.size(), dotted) == 0) {
        stem.resize(stem.size() - dotted.size());
    }
    std::string key;
    key.reserve(stem.size());
    for (char character : stem) {
        key.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
    }
    return key;
}

std::string lrps2_upscale_value(int scale) {
    static constexpr const char* kValues[] = {
        "1x Native (PS2)",
        "2x Native (~720p)",
        "3x Native (~1080p)",
        "4x Native (~1440p/2K)",
        "5x Native (~1800p/3K)",
        "6x Native (~2160p/4K)",
    };
    const auto index = std::clamp(scale, 1, 6) - 1;
    return kValues[index];
}

std::string ppsspp_resolution_value(int scale) {
    const auto n = std::clamp(scale, 1, 6);
    return std::to_string(480 * n) + "x" + std::to_string(272 * n);
}

std::string citra_resolution_value(int scale) {
    const auto n = std::clamp(scale, 1, 6);
    if (n == 1) {
        return "1x (Native)";
    }
    return std::to_string(n) + "x";
}

std::string beetle_hw_resolution_value(int scale) {
    // Beetle PSX HW only exposes 1x / 2x / 4x / 8x / 16x.
    const auto n = std::clamp(scale, 1, 6);
    if (n <= 1) {
        return "1x(native)";
    }
    if (n == 2) {
        return "2x";
    }
    if (n <= 4) {
        return "4x";
    }
    return "8x";
}

// LRPS2 defaults to Auto/Vulkan which often paints black under Xvfb.
void ensure_lrps2_virtual_display_options() {
    const auto path = retroarch_core_opt_path("LRPS2");
    if (path.empty()) {
        return;
    }
    upsert_core_opt_file(path, {
        {"pcsx2_renderer", "OpenGL"},
        {"pcsx2_fastboot", "enabled"},
    });
}

void apply_retroarch_resolution_scale(
    const std::filesystem::path& core_path,
    int resolution_scale) {
    if (core_path.empty()) {
        return;
    }
    const auto scale = std::clamp(resolution_scale, 1, 6);
    const auto key = core_file_key(core_path);
    const auto scale_str = std::to_string(scale);

    auto write = [&](std::string_view dir, std::vector<std::pair<std::string, std::string>> opts) {
        const auto path = retroarch_core_opt_path(dir);
        if (!path.empty()) {
            upsert_core_opt_file(path, opts);
        }
    };

    if (key == "pcsx2" || key == "lrps2") {
        write("LRPS2", {{"pcsx2_upscale_multiplier", lrps2_upscale_value(scale)}});
    } else if (key == "swanstation" || key == "duckstation") {
        write("SwanStation", {{"swanstation_GPU_ResolutionScale", scale_str}});
    } else if (key == "ppsspp") {
        write("PPSSPP", {{"ppsspp_internal_resolution", ppsspp_resolution_value(scale)}});
    } else if (key == "dolphin") {
        // Distros disagree on corename ("Dolphin" vs "dolphin-emu"); update both.
        write("dolphin-emu", {{"dolphin_efb_scale", scale_str}});
        write("Dolphin", {{"dolphin_efb_scale", scale_str}});
    } else if (key == "citra" || key == "citra_canary" || key == "citra2018") {
        write("Citra", {{"citra_resolution_factor", citra_resolution_value(scale)}});
    } else if (key == "mupen64plus_next" || key == "mupen64plus-next") {
        write("Mupen64Plus-Next", {
            {"mupen64plus-EnableNativeResFactor", scale_str},
        });
    } else if (key == "mednafen_psx_hw" || key == "beetle_psx_hw") {
        write("Beetle PSX HW", {
            {"beetle_psx_hw_internal_resolution", beetle_hw_resolution_value(scale)},
        });
    }
}

} // namespace

VirtualGamepadIdentity identity_for_port(
    const std::vector<VirtualGamepadIdentity>& identities,
    RetroArchPort port) {
    if (port < identities.size()) {
        auto identity = identities[port];
        if (identity.name.empty()) {
            identity.name = "ArchStreamer Virtual Gamepad";
        }
        return identity;
    }

    return VirtualGamepadIdentity{};
}

std::filesystem::path write_retroarch_input_override(
    std::size_t first_virtual_joypad_index,
    const std::vector<VirtualGamepadIdentity>& identities,
    const std::string& joypad_driver,
    RetroArchPort players,
    const SaveProfile& save_profile,
    bool realtime_pacing,
    bool capture_fullscreen,
    std::string_view capture_resolution,
    int vulkan_gpu_index,
    std::string_view system_key,
    const std::filesystem::path& core_path,
    int resolution_scale) {
    // Home path so Flatpak ArchStreamer + flatpak-spawn --host retroarch share the same files.
    const auto root = retroarch_runtime_root();
    const auto directory = root / "config";
    std::filesystem::create_directories(directory);

    const auto autoconfig_directory = root / "autoconfig";
    std::filesystem::remove_all(autoconfig_directory);
    std::filesystem::create_directories(autoconfig_directory / joypad_driver);

    const auto face = face_button_indices_for_system(system_key);
    for (RetroArchPort port = 0; port < players; ++port) {
        write_virtual_pad_autoconfig(
            autoconfig_directory,
            identity_for_port(identities, port),
            joypad_driver,
            port,
            face);
    }
    const auto path = directory / "input_override.cfg";
    std::ofstream file(path, std::ios::trunc);
    if (!file) {
        throw std::runtime_error("failed to write RetroArch input override");
    }

    // Prefer the same BIOS/system tree RetroArch / ra.py use (PS1 SCPH*.bin, etc.).
    std::filesystem::path system_directory;
    if (const auto home = user_home_directory(); !home.empty()) {
        system_directory = std::filesystem::path(home) / ".config/retroarch/system";
    }

    file
        << "config_save_on_exit = \"false\"\n"
        << "input_joypad_driver = \"" << joypad_driver << "\"\n"
        << "input_max_users = \"" << static_cast<int>(players) << "\"\n"
        << "input_autodetect_enable = \"true\"\n"
        << "notification_show_autoconfig = \"false\"\n"
        << "joypad_autoconfig_dir = \"" << autoconfig_directory.string() << "\"\n";
    if (!system_directory.empty()) {
        file << "system_directory = \"" << system_directory.string() << "\"\n";
    }

    file
        << "savefile_directory = \"" << save_profile.savefile_directory.string() << "\"\n"
        << "savestate_directory = \"" << save_profile.savestate_directory.string() << "\"\n"
        << "sort_savefiles_enable = \"false\"\n"
        << "sort_savefiles_by_content_enable = \"false\"\n"
        << "sort_savestates_enable = \"false\"\n"
        << "sort_savestates_by_content_enable = \"false\"\n"
        // Input is driven through ArchStreamer virtual pads (host bridge or UDP).
        // RetroArch's default pause_nonactive=true makes Host Player look like a
        // dead controller as soon as the GUI or another window takes focus.
        << "pause_nonactive = \"false\"\n"
        << "network_cmd_enable = \"true\"\n"
        << "network_cmd_port = \"" << DefaultRetroArchNetcmdPort << "\"\n"
        // Virtual keyboard from clients: Space holds fast-forward; F1 opens menu.
        // Disable the enable-hotkey chord so kids do not need a modifier.
        // Note: official key is input_toggle_fast_forward (underscore), not …fastforward.
        << "input_enable_hotkey = \"nul\"\n"
        << "input_hold_fast_forward = \"space\"\n"
        << "input_toggle_fast_forward = \"nul\"\n"
        << "input_menu_toggle = \"f1\"\n"
        << "fastforward_ratio = \"3.0\"\n"
        << "fastforward_frameskip = \"true\"\n";

    if (realtime_pacing) {
        // Known-good streaming pacing (do not over-tune — host Watch-local used to be fine
        // with just these knobs feeding the null-sink → Opus path).
        // Force GL on the virtual X display: Vulkan/Auto HW cores (LRPS2/PCSX2) often
        // present a black framebuffer to ximagesrc under Xvfb.
        // Keep audio_sync on so RetroArch paces the core (audio_sync=false made PS2 run fast
        // and input feel unreliable). Capture still listens on archstreamer.monitor.
        file
            << "video_driver = \"gl\"\n"
            // Prefer X11 context on the virtual capture display. Without this (and with a
            // live host Wayland session), RetroArch glcore attaches to the compositor —
            // game visible on the host, black ximagesrc for clients.
            << "video_context_driver = \"x11\"\n"
            << "audio_enable = \"true\"\n"
            << "audio_mute = \"false\"\n"
            << "audio_driver = \"pulse\"\n"
            << "audio_sync = \"true\"\n"
            << "video_vsync = \"false\"\n"
            << "runahead_enabled = \"false\"\n";
    } else {
        // Host Player on the real display. Prefer Vulkan with an explicit GPU index when
        // known (multi-GPU). Streaming (:99) keeps the gl path via realtime_pacing.
        file << "video_driver = \"vulkan\"\n";
        if (vulkan_gpu_index >= 0) {
            file << "vulkan_gpu_index = \"" << vulkan_gpu_index << "\"\n";
        }
        file
            << "audio_enable = \"true\"\n"
            << "audio_mute = \"false\"\n"
            << "audio_driver = \"pulse\"\n";
    }

    if (capture_fullscreen) {
        // Fill the virtual capture display so remotes don't see a tiny windowed corner.
        int width = 1280;
        int height = 720;
        const auto x_pos = capture_resolution.find('x');
        if (x_pos != std::string_view::npos) {
            try {
                width = std::stoi(std::string(capture_resolution.substr(0, x_pos)));
                height = std::stoi(std::string(capture_resolution.substr(x_pos + 1)));
            } catch (const std::exception&) {
            }
        }
        file
            << "video_fullscreen = \"true\"\n"
            << "video_windowed_fullscreen = \"false\"\n"
            << "video_force_resolution = \"true\"\n"
            << "video_fullscreen_x = \"" << width << "\"\n"
            << "video_fullscreen_y = \"" << height << "\"\n"
            << "video_window_show_decor = \"false\"\n"
            << "video_font_enable = \"false\"\n"
            << "menu_enable = \"false\"\n";
    }

    for (RetroArchPort port = 0; port < players; ++port) {
        const auto player = static_cast<int>(port) + 1;
        const auto joypad_index = first_virtual_joypad_index + port;
        file
            << "input_player" << player << "_joypad_index = \"" << joypad_index << "\"\n"
            << "input_player" << player << "_b_btn = \"" << face.b << "\"\n"
            << "input_player" << player << "_a_btn = \"" << face.a << "\"\n"
            << "input_player" << player << "_y_btn = \"" << face.y << "\"\n"
            << "input_player" << player << "_x_btn = \"" << face.x << "\"\n"
            << "input_player" << player << "_l_btn = \"4\"\n"
            << "input_player" << player << "_r_btn = \"5\"\n"
            << "input_player" << player << "_select_btn = \"6\"\n"
            << "input_player" << player << "_start_btn = \"7\"\n"
            << "input_player" << player << "_l3_btn = \"9\"\n"
            << "input_player" << player << "_r3_btn = \"10\"\n"
            << "input_player" << player << "_l2_axis = \"+2\"\n"
            << "input_player" << player << "_r2_axis = \"+5\"\n"
            << "input_player" << player << "_l_x_minus_axis = \"-0\"\n"
            << "input_player" << player << "_l_x_plus_axis = \"+0\"\n"
            << "input_player" << player << "_l_y_minus_axis = \"-1\"\n"
            << "input_player" << player << "_l_y_plus_axis = \"+1\"\n"
            << "input_player" << player << "_r_x_minus_axis = \"-3\"\n"
            << "input_player" << player << "_r_x_plus_axis = \"+3\"\n"
            << "input_player" << player << "_r_y_minus_axis = \"-4\"\n"
            << "input_player" << player << "_r_y_plus_axis = \"+4\"\n"
            << "input_player" << player << "_up_btn = \"11\"\n"
            << "input_player" << player << "_down_btn = \"12\"\n"
            << "input_player" << player << "_left_btn = \"13\"\n"
            << "input_player" << player << "_right_btn = \"14\"\n";
    }

    if (realtime_pacing || capture_fullscreen) {
        ensure_lrps2_virtual_display_options();
    }
    apply_retroarch_resolution_scale(core_path, resolution_scale);

    return path;
}

} // namespace archstreamer
