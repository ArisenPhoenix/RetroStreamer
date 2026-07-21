#include "host/retroarch_config_writer.hpp"

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace archstreamer {

namespace {

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

std::filesystem::path write_virtual_pad_autoconfig(
    const VirtualGamepadIdentity& identity,
    const std::string& joypad_driver,
    RetroArchPort port) {
    const auto directory = std::filesystem::temp_directory_path() / "archstreamer-retroarch-autoconfig" / joypad_driver;

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
        << "input_b_btn = \"0\"\n"
        << "input_a_btn = \"1\"\n"
        << "input_y_btn = \"2\"\n"
        << "input_x_btn = \"3\"\n"
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
        << "input_right_btn = \"14\"\n"
        << "input_left_axis = \"-6\"\n"
        << "input_right_axis = \"+6\"\n"
        << "input_up_axis = \"-7\"\n"
        << "input_down_axis = \"+7\"\n";

    return directory.parent_path();
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
    bool realtime_pacing) {
    const auto directory = std::filesystem::temp_directory_path() / "archstreamer-retroarch-config";
    std::filesystem::create_directories(directory);

    const auto autoconfig_directory =
        std::filesystem::temp_directory_path() / "archstreamer-retroarch-autoconfig";
    const auto autoconfig_driver_directory = autoconfig_directory / joypad_driver;
    std::filesystem::remove_all(autoconfig_directory);
    std::filesystem::create_directories(autoconfig_driver_directory);

    for (RetroArchPort port = 0; port < players; ++port) {
        write_virtual_pad_autoconfig(identity_for_port(identities, port), joypad_driver, port);
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

    for (RetroArchPort port = 0; port < players; ++port) {
        const auto player = static_cast<int>(port) + 1;
        const auto joypad_index = first_virtual_joypad_index + port;
        file
            << "input_player" << player << "_joypad_index = \"" << joypad_index << "\"\n"
            << "input_player" << player << "_b_btn = \"0\"\n"
            << "input_player" << player << "_a_btn = \"1\"\n"
            << "input_player" << player << "_y_btn = \"2\"\n"
            << "input_player" << player << "_x_btn = \"3\"\n"
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
            << "input_player" << player << "_right_btn = \"14\"\n"
            << "input_player" << player << "_left_axis = \"-6\"\n"
            << "input_player" << player << "_right_axis = \"+6\"\n"
            << "input_player" << player << "_up_axis = \"-7\"\n"
            << "input_player" << player << "_down_axis = \"+7\"\n";
    }

    return path;
}

} // namespace archstreamer
