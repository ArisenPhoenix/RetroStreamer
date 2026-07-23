#include "host/retroarch_config_writer.hpp"

#include "common/platform/paths.hpp"

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>

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

void write_virtual_pad_autoconfig(
    const std::filesystem::path& autoconfig_root,
    const VirtualGamepadIdentity& identity,
    const std::string& joypad_driver,
    RetroArchPort port) {
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
        << "input_b_btn = \"1\"\n"
        << "input_a_btn = \"0\"\n"
        << "input_y_btn = \"3\"\n"
        << "input_x_btn = \"2\"\n"
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
        // sdl2 exposes ABS_HAT0 as hat 0, not as axes 6/7.
        << "input_up_btn = \"h0up\"\n"
        << "input_down_btn = \"h0down\"\n"
        << "input_left_btn = \"h0left\"\n"
        << "input_right_btn = \"h0right\"\n";
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
    std::string_view capture_resolution) {
    // Home path so Flatpak ArchStreamer + flatpak-spawn --host retroarch share the same files.
    const auto root = retroarch_runtime_root();
    const auto directory = root / "config";
    std::filesystem::create_directories(directory);

    const auto autoconfig_directory = root / "autoconfig";
    std::filesystem::remove_all(autoconfig_directory);
    std::filesystem::create_directories(autoconfig_directory / joypad_driver);

    for (RetroArchPort port = 0; port < players; ++port) {
        write_virtual_pad_autoconfig(
            autoconfig_directory,
            identity_for_port(identities, port),
            joypad_driver,
            port);
    }
    const auto path = directory / "input_override.cfg";
    std::ofstream file(path, std::ios::trunc);
    if (!file) {
        throw std::runtime_error("failed to write RetroArch input override");
    }

    file
        << "config_save_on_exit = \"false\"\n"
        << "input_joypad_driver = \"" << joypad_driver << "\"\n"
        << "input_max_users = \"" << static_cast<int>(players) << "\"\n"
        << "input_autodetect_enable = \"true\"\n"
        << "notification_show_autoconfig = \"false\"\n"
        << "joypad_autoconfig_dir = \"" << autoconfig_directory.string() << "\"\n";

    file
        << "savefile_directory = \"" << save_profile.savefile_directory.string() << "\"\n"
        << "savestate_directory = \"" << save_profile.savestate_directory.string() << "\"\n"
        << "sort_savefiles_enable = \"false\"\n"
        << "sort_savefiles_by_content_enable = \"false\"\n"
        << "sort_savestates_enable = \"false\"\n"
        << "sort_savestates_by_content_enable = \"false\"\n";

    if (realtime_pacing) {
        file
            << "audio_enable = \"true\"\n"
            << "audio_mute = \"false\"\n"
            << "audio_driver = \"pulse\"\n"
            << "audio_sync = \"true\"\n"
            << "video_vsync = \"false\"\n"
            << "runahead_enabled = \"false\"\n";
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
            << "menu_enable = \"false\"\n"
            << "pause_nonactive = \"false\"\n";
    }

    for (RetroArchPort port = 0; port < players; ++port) {
        const auto player = static_cast<int>(port) + 1;
        const auto joypad_index = first_virtual_joypad_index + port;
        file
            << "input_player" << player << "_joypad_index = \"" << joypad_index << "\"\n"
            << "input_player" << player << "_b_btn = \"1\"\n"
            << "input_player" << player << "_a_btn = \"0\"\n"
            << "input_player" << player << "_y_btn = \"3\"\n"
            << "input_player" << player << "_x_btn = \"2\"\n"
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
            << "input_player" << player << "_up_btn = \"h0up\"\n"
            << "input_player" << player << "_down_btn = \"h0down\"\n"
            << "input_player" << player << "_left_btn = \"h0left\"\n"
            << "input_player" << player << "_right_btn = \"h0right\"\n";
    }

    return path;
}

} // namespace archstreamer
