#include "host/switch/yuzu_controls.hpp"

#include "host/switch/qt_ini_editor.hpp"
#include "host/switch/yuzu_user_profile.hpp"

#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <stdexcept>

namespace archstreamer {
namespace {

std::string sdl_button_binding(const std::string& guid, int button) {
    return "pad:0,button:" + std::to_string(button) + ",port:0,guid:" + guid + ",engine:sdl";
}

std::string sdl_axis_trigger_binding(const std::string& guid, int axis) {
    return "threshold:0.500000,axis:" + std::to_string(axis) + ",port:0,guid:" + guid +
        ",invert:+,engine:sdl";
}

std::string sdl_stick_binding(const std::string& guid, int axis_x, int axis_y) {
    return "range:1.000000,deadzone:0.150000,threshold:0.500000,axis_y:" + std::to_string(axis_y) +
        ",axis_x:" + std::to_string(axis_x) + ",pad:0,port:0,guid:" + guid + ",engine:sdl";
}

void write_yuzu_sdl_binding_map(
    const std::function<void(std::string_view, std::string_view)>& set,
    const std::string& guid) {
    set("button_a", sdl_button_binding(guid, 0));
    set("button_b", sdl_button_binding(guid, 1));
    set("button_x", sdl_button_binding(guid, 2));
    set("button_y", sdl_button_binding(guid, 3));
    set("button_l", sdl_button_binding(guid, 4));
    set("button_r", sdl_button_binding(guid, 5));
    set("button_minus", sdl_button_binding(guid, 6));
    set("button_plus", sdl_button_binding(guid, 7));
    set("button_home", sdl_button_binding(guid, 8));
    set("button_lstick", sdl_button_binding(guid, 9));
    set("button_rstick", sdl_button_binding(guid, 10));
    set("button_dup", sdl_button_binding(guid, 11));
    set("button_ddown", sdl_button_binding(guid, 12));
    set("button_dleft", sdl_button_binding(guid, 13));
    set("button_dright", sdl_button_binding(guid, 14));
    set("button_sl", sdl_button_binding(guid, 4));
    set("button_sr", sdl_button_binding(guid, 5));
    set("button_zl", sdl_axis_trigger_binding(guid, 2));
    set("button_zr", sdl_axis_trigger_binding(guid, 5));
    set("button_screenshot", "[empty]");
    set("lstick", sdl_stick_binding(guid, 0, 1));
    set("rstick", sdl_stick_binding(guid, 3, 4));
    set("motionleft", "[empty]");
    set("motionright", "[empty]");
    set("type", "0");
}

void write_yuzu_player_sdl_controls(std::string& contents, int player_index, const std::string& guid) {
    const std::string prefix = "player_" + std::to_string(player_index) + "_";
    const auto set = [&](std::string_view suffix, std::string_view value) {
        set_qt_ini_group_value(contents, "Controls", std::string(prefix) + std::string(suffix), value);
        set_qt_ini_group_value(
            contents,
            "Controls",
            std::string(prefix) + std::string(suffix) + "\\default",
            "false");
    };

    write_yuzu_sdl_binding_map(set, guid);
    set("connected", "true");
}

void write_yuzu_input_profile(const std::filesystem::path& path, const std::string& guid) {
    std::string contents = "[Controls]\n";
    const auto set = [&](std::string_view key, std::string_view value) {
        set_qt_ini_value(contents, key, value);
        set_qt_ini_value(contents, std::string(key) + "\\default", "false");
    };
    write_yuzu_sdl_binding_map(set, guid);
    set_qt_ini_value(contents, "type\\default", "true");

    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::trunc);
    if (!out) {
        throw std::runtime_error("failed to write Yuzu input profile: " + path.string());
    }
    out << contents;
}

} // namespace

void YuzuControls::configure_archstreamer_controls(
    const YuzuUserProfile& profile,
    const std::vector<std::string>& sdl_guids) {
    if (sdl_guids.empty()) {
        std::cerr << "Warning: no ArchStreamer SDL GUIDs; Yuzu Controls left unchanged.\n";
        return;
    }

    const auto config_dir = profile.xdg_config_home / "yuzu";
    const auto config_path = config_dir / "qt-config.ini";
    const auto input_dir = config_dir / "input";
    std::filesystem::create_directories(input_dir);

    std::string contents;
    if (std::filesystem::exists(config_path)) {
        std::ifstream in(config_path);
        contents.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    if (contents.empty()) {
        YuzuUserProfileService::ensure_qt_config(profile, false, false, -1, 0);
        std::ifstream in(config_path);
        contents.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }

    set_qt_ini_group_value(contents, "Controls", "keyboard_enabled", "false");
    set_qt_ini_group_value(contents, "Controls", "keyboard_enabled\\default", "false");

    const auto filter_guid = [](std::string guid) {
        if (guid.size() >= 8) {
            guid.replace(4, 4, "0000");
        }
        return guid;
    };

    for (std::size_t i = 0; i < sdl_guids.size() && i < 8; ++i) {
        if (sdl_guids[i].empty()) {
            continue;
        }
        const auto guid = filter_guid(sdl_guids[i]);
        const auto profile_name =
            i == 0 ? std::string("ArchStreamer") : ("ArchStreamer_P" + std::to_string(i + 1));
        write_yuzu_input_profile(input_dir / (profile_name + ".ini"), guid);
        write_yuzu_player_sdl_controls(contents, static_cast<int>(i), guid);
    }

    std::ofstream out(config_path, std::ios::trunc);
    if (!out) {
        throw std::runtime_error("failed to write Yuzu Controls: " + config_path.string());
    }
    out << contents;
    std::cout << "Yuzu Controls: bound " << sdl_guids.size()
              << " ArchStreamer pad(s) via SDL GUID (+ input/ profile)\n";
}

} // namespace archstreamer
