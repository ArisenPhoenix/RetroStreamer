#include "host/retroarch_config_writer.hpp"

#include "common/platform/paths.hpp"
#include "host/nds_display_layout.hpp"
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
//
// Linux udev indexes EV_KEY in ascending code order. With our uinput pad
// (BTN_C not advertised) that is:
//   0=SOUTH, 1=EAST, 2=NORTH, 3=WEST
// NOT the common SDL face order (2=WEST, 3=NORTH). Mixing those up swaps
// Square/Triangle on PS (and X/Y on Nintendo) while leaving Cross/Circle OK.
//
// Host is a dumb NESW relay: South→RetroPad A, East→RetroPad B, West→RetroPad Y,
// North→RetroPad X (Xbox/SDL letter layout). Per-system or DualShock quirks are
// fixed on the client via Swap NW / Swap SE — not here.
struct FaceButtonIndices {
    const char* b = "1"; // EAST
    const char* a = "0"; // SOUTH
    const char* y = "2"; // NORTH
    const char* x = "3"; // WEST
    const char* name = "NESW letter map";
};

FaceButtonIndices face_button_indices_for_system(std::string_view /*system_key*/) {
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

// HW-rendered libretro cores draw into an FBO; RetroArch's gl driver must present
// that to X11. Software cores (GB, etc.) only upload CPU frames — gl/Xvfb often
// leaves ximagesrc on a stale still until continuous animation, so remotes freeze
// on title/credits until something starts moving. sdl2 PutImage's every present.
bool core_needs_gl_on_virtual_display_impl(const std::filesystem::path& core_path) {
    if (core_path.empty()) {
        return false;
    }
    const auto key = core_file_key(core_path);
    return key == "pcsx2" || key == "lrps2" || key == "dolphin" || key == "ppsspp" ||
        key == "citra" || key == "citra_canary" || key == "citra2018" ||
        key == "mupen64plus_next" || key == "mupen64plus-next" ||
        key == "mednafen_psx_hw" || key == "beetle_psx_hw" ||
        key == "flycast" || key == "parallel_n64" || key == "yabause" ||
        key == "kronos" || key == "desmume" || key == "melonds" ||
        key == "melondsds";
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

bool core_needs_gl_on_virtual_display(const std::filesystem::path& core_path) {
    return core_needs_gl_on_virtual_display_impl(core_path);
}

// Stream-friendly DS layout. melonDS "Hybrid Top" matches landscape/desktop;
// "Top/Bottom" stacks equal screens for portrait phones.
void apply_nds_screen_layout(DisplayLayoutPreference preference) {
    auto write = [&](std::string_view dir, std::vector<std::pair<std::string, std::string>> opts) {
        const auto path = retroarch_core_opt_path(dir);
        if (!path.empty()) {
            upsert_core_opt_file(path, opts);
        }
    };

    const auto layout = resolve_nds_display_layout(preference);
    write("melonDS", {
        {"melonds_screen_layout", layout.core_layout},
        {"melonds_hybrid_small_screen", "Bottom"},
        {"melonds_hybrid_ratio", "3"},
        {"melonds_screen_gap", "0"},
        {"melonds_swapscreen_mode", "Toggle"},
        {"melonds_opengl_renderer", "enabled"},
        // Layout buffer is still ≥4× native in-core; keep 3D scale modest.
        {"melonds_opengl_resolution", "2x native (512x384)"},
    });
}

std::string_view face_button_map_name(std::string_view system_key) {
    return face_button_indices_for_system(system_key).name;
}

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
    int resolution_scale,
    int slot_index,
    std::uint16_t network_cmd_port,
    DisplayLayoutPreference display_layout) {
    // Home path so Flatpak ArchStreamer + flatpak-spawn --host retroarch share the same files.
    const auto root = retroarch_runtime_root();
    const auto directory = root / "config";
    std::filesystem::create_directories(directory);

    const auto autoconfig_directory =
        root / ("autoconfig_slot" + std::to_string(std::max(0, slot_index)));
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
    const auto path =
        directory / ("input_override_slot" + std::to_string(std::max(0, slot_index)) + ".cfg");
    std::ofstream file(path, std::ios::trunc);
    if (!file) {
        throw std::runtime_error("failed to write RetroArch input override");
    }

    // Per-user mirror of the BIOS/system tree RetroArch / ra.py use (PS1 SCPH*.bin,
    // etc.). Everything is symlinked back to the shared tree except pcsx2/memcards,
    // so concurrent sessions cannot open each other's PS2 cards.
    std::filesystem::path system_directory = save_profile.system_directory;
    if (system_directory.empty()) {
        system_directory = shared_retroarch_system_directory();
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
        << "network_cmd_port = \"" << network_cmd_port << "\"\n"
        // Localhost GBA link (gpSP netpacket) must not hit public lobby / MITM / UPnP.
        << "netplay_public_announce = \"false\"\n"
        << "netplay_use_mitm_server = \"false\"\n"
        << "netplay_nat_traversal = \"false\"\n"
        << "netplay_allow_slaves = \"false\"\n"
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
        // HW cores need gl + x11 context on the virtual display (Vulkan paints black to
        // ximagesrc under Xvfb). Software cores use sdl2 so every present hits X11 via
        // PutImage — gl/Xvfb was leaving remotes on a stale still through GB credits
        // until the title Pokemon started animating.
        // Keep audio_sync on so RetroArch paces the core (audio_sync=false made PS2 run fast
        // and input feel unreliable). Capture still listens on archstreamer.monitor.
        const bool need_gl = core_needs_gl_on_virtual_display_impl(core_path);
        if (need_gl) {
            file
                << "video_driver = \"gl\"\n"
                << "video_context_driver = \"x11\"\n";
        } else {
            file << "video_driver = \"sdl2\"\n";
        }
        file
            << "audio_enable = \"true\"\n"
            << "audio_mute = \"false\"\n"
            << "audio_driver = \"pulse\"\n"
            << "audio_sync = \"true\"\n"
            // Keep Pulse latency modest: audio_sync paces the core to this buffer, so
            // 64 ms here alone made every button feel a frame late before capture.
            << "audio_latency = \"32\"\n"
            << "video_vsync = \"false\"\n"
            << "runahead_enabled = \"false\"\n";
        // framecount_show stays off by default. Clients can request a ticking
        // Frames OSD mid-session via ViewerHeartbeat.show_framecount (SHOW_MSG).
        if (need_gl) {
            file << "video_font_enable = \"true\"\n";
        }
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
            << "menu_enable = \"false\"\n";
        // Keep fonts available for optional client-requested Frames OSD (SHOW_MSG).
        // Software/sdl2 streaming still prefers a clean capture when fonts stay off.
        if (!(realtime_pacing && core_needs_gl_on_virtual_display_impl(core_path))) {
            file << "video_font_enable = \"false\"\n";
        }
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
    if (core_file_key(core_path) == "melonds" || system_key == "nds") {
        apply_nds_screen_layout(display_layout);
    }

    return path;
}

} // namespace archstreamer
