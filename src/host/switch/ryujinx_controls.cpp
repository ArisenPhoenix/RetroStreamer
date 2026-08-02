#include "host/switch/ryujinx_controls.hpp"

#include <nlohmann/json.hpp>

#include <fstream>
#include <iostream>
#include <stdexcept>

namespace archstreamer {
namespace {

std::string ryujinx_device_id_from_sdl_guid(std::size_t index, const std::string& sdl_guid) {
    if (sdl_guid.size() < 32) {
        return std::to_string(index) + "-" + sdl_guid;
    }
    auto byte = [&](std::size_t i) { return sdl_guid.substr(i * 2, 2); };
    const std::string a = byte(3) + byte(2) + byte(1) + byte(0);
    const std::string b = byte(5) + byte(4);
    const std::string c = byte(7) + byte(6);
    const std::string d = byte(8) + byte(9);
    const std::string e = byte(10) + byte(11) + byte(12) + byte(13) + byte(14) + byte(15);
    return std::to_string(index) + "-" + a + "-" + b + "-" + c + "-" + d + "-" + e;
}

nlohmann::json ryujinx_pro_controller_sdl_binding(
    std::size_t player_index,
    const ArchStreamerSdlPad& pad,
    const std::string& pad_name) {
    return {
        {"left_joycon_stick",
         {{"joystick", "Left"},
          {"invert_stick_x", false},
          {"invert_stick_y", false},
          {"rotate90_cw", false},
          {"stick_button", "LeftStick"}}},
        {"right_joycon_stick",
         {{"joystick", "Right"},
          {"invert_stick_x", false},
          {"invert_stick_y", false},
          {"rotate90_cw", false},
          {"stick_button", "RightStick"}}},
        {"deadzone_left", 0.1},
        {"deadzone_right", 0.1},
        {"range_left", 1.0},
        {"range_right", 1.0},
        {"trigger_threshold", 0.5},
        {"motion",
         {{"motion_backend", "GamepadDriver"},
          {"sensitivity", 100},
          {"gyro_deadzone", 1},
          {"enable_motion", false}}},
        {"rumble", {{"strong_rumble", 1.0}, {"weak_rumble", 1.0}, {"enable_rumble", false}}},
        {"led",
         {{"enable_led", false},
          {"turn_off_led", false},
          {"use_rainbow", false},
          {"led_color", 0}}},
        {"left_joycon",
         {{"button_minus", "Back"},
          {"button_l", "LeftShoulder"},
          {"button_zl", "LeftTrigger"},
          {"button_sl", "Unbound"},
          {"button_sr", "Unbound"},
          {"dpad_up", "DpadUp"},
          {"dpad_down", "DpadDown"},
          {"dpad_left", "DpadLeft"},
          {"dpad_right", "DpadRight"}}},
        {"right_joycon",
         {{"button_plus", "Start"},
          {"button_r", "RightShoulder"},
          {"button_zr", "RightTrigger"},
          {"button_sl", "Unbound"},
          {"button_sr", "Unbound"},
          {"button_x", "X"},
          {"button_b", "B"},
          {"button_y", "Y"},
          {"button_a", "A"}}},
        {"version", 1},
        {"backend", "GamepadSDL2"},
        // Ryujinx keys gamepads on "<SDL joystick index>-<GUID>", so the index has to be
        // the one its SDL enumeration will hand out, not the player slot.
        {"id", ryujinx_device_id_from_sdl_guid(pad.sdl_index, pad.guid)},
        {"name", pad_name},
        {"controller_type", "ProController"},
        {"player_index", "Player" + std::to_string(player_index + 1)},
    };
}

std::string archstreamer_sdl_gamecontroller_mapping(const std::string& sdl_guid, const std::string& name) {
    return sdl_guid + "," + name +
        ",platform:Linux,"
        "a:b0,b:b1,y:b2,x:b3,"
        "leftshoulder:b4,rightshoulder:b5,"
        "back:b6,start:b7,guide:b8,"
        "leftstick:b9,rightstick:b10,"
        "dpup:b11,dpdown:b12,dpleft:b13,dpright:b14,"
        "leftx:a0,lefty:a1,lefttrigger:a2,rightx:a3,righty:a4,righttrigger:a5";
}

} // namespace

void RyujinxControls::configure_archstreamer_controls(
    RyujinxUserProfile& profile,
    const std::vector<ArchStreamerSdlPad>& pads,
    const std::string& sdl_device_filter) {
    if (pads.empty()) {
        std::cerr << "Warning: no ArchStreamer SDL pads; Ryujinx input_config left unchanged.\n";
        return;
    }

    const auto config_path = profile.data_root / "Config.json";
    nlohmann::json cfg = nlohmann::json::object();
    if (std::filesystem::is_regular_file(config_path)) {
        try {
            std::ifstream in(config_path);
            cfg = nlohmann::json::parse(in, nullptr, /*allow_exceptions=*/true);
            if (!cfg.is_object()) {
                cfg = nlohmann::json::object();
            }
        } catch (const nlohmann::json::exception&) {
            cfg = nlohmann::json::object();
        }
    }

    nlohmann::json input = nlohmann::json::array();
    std::string mappings;
    for (std::size_t i = 0; i < pads.size() && i < 8; ++i) {
        const auto& pad = pads[i];
        if (pad.guid.empty()) {
            continue;
        }
        const auto name =
            i == 0 ? std::string("ArchStreamer") : ("ArchStreamer_P" + std::to_string(i + 1));
        input.push_back(ryujinx_pro_controller_sdl_binding(i, pad, name));
        if (!mappings.empty()) {
            mappings.push_back('\n');
        }
        mappings += archstreamer_sdl_gamecontroller_mapping(
            pad.mapping_guid.empty() ? pad.guid : pad.mapping_guid, name);
    }
    cfg["input_config"] = std::move(input);
    cfg["use_input_global_config"] = false;
    cfg["disable_input_when_out_of_focus"] = false;

    std::ofstream out(config_path, std::ios::trunc);
    if (!out) {
        throw std::runtime_error("failed to write Ryujinx input_config: " + config_path.string());
    }
    out << cfg.dump(2) << '\n';
    profile.sdl_gamecontroller_config = std::move(mappings);
    profile.sdl_device_filter = sdl_device_filter;
    std::cout << "Ryujinx Controls: bound " << pads.size()
              << " ArchStreamer pad(s) via GamepadSDL2 (+ SDL_GAMECONTROLLERCONFIG), sdl index "
              << pads.front().sdl_index << (sdl_device_filter.empty() ? " (unfiltered)" : " (exclusive)")
              << '\n';
}

} // namespace archstreamer
