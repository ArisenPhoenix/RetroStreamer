#include "common/controller_button_map.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>

namespace archstreamer {
namespace {

constexpr std::uint16_t kTriggerDownThreshold = 6554; // ~0.1 * 65535

std::string lower_ascii(std::string_view in) {
    std::string out;
    out.reserve(in.size());
    for (unsigned char ch : in) {
        out.push_back(static_cast<char>(std::tolower(ch)));
    }
    return out;
}

bool source_down(const ControllerState& state, ControllerMapSource source) {
    switch (source) {
    case ControllerMapSource::Select:
        return (state.buttons & ButtonBack) != 0;
    case ControllerMapSource::Start:
        return (state.buttons & ButtonStart) != 0;
    case ControllerMapSource::L:
        return (state.buttons & ButtonLeftShoulder) != 0;
    case ControllerMapSource::R:
        return (state.buttons & ButtonRightShoulder) != 0;
    case ControllerMapSource::L2:
        return state.left_trigger > kTriggerDownThreshold;
    case ControllerMapSource::R2:
        return state.right_trigger > kTriggerDownThreshold;
    case ControllerMapSource::L3:
        return (state.buttons & ButtonLeftStick) != 0;
    case ControllerMapSource::R3:
        return (state.buttons & ButtonRightStick) != 0;
    }
    return false;
}

std::uint16_t source_analog_level(const ControllerState& state, ControllerMapSource source) {
    switch (source) {
    case ControllerMapSource::L2:
        return state.left_trigger;
    case ControllerMapSource::R2:
        return state.right_trigger;
    default:
        return 0xFFFF;
    }
}

void apply_action(
    ControllerState& out,
    ControllerMapAction action,
    bool down,
    std::uint16_t analog_level,
    bool& ff_held,
    bool& menu_down,
    bool& screen_swap_down) {
    if (!down) {
        return;
    }
    switch (action) {
    case ControllerMapAction::Default:
        break;
    case ControllerMapAction::A:
        out.buttons |= ButtonA;
        break;
    case ControllerMapAction::B:
        out.buttons |= ButtonB;
        break;
    case ControllerMapAction::X:
        out.buttons |= ButtonX;
        break;
    case ControllerMapAction::Y:
        out.buttons |= ButtonY;
        break;
    case ControllerMapAction::L:
        out.buttons |= ButtonLeftShoulder;
        break;
    case ControllerMapAction::R:
        out.buttons |= ButtonRightShoulder;
        break;
    case ControllerMapAction::L2:
        out.left_trigger = std::max(out.left_trigger, analog_level);
        break;
    case ControllerMapAction::R2:
        out.right_trigger = std::max(out.right_trigger, analog_level);
        break;
    case ControllerMapAction::Select:
        out.buttons |= ButtonBack;
        break;
    case ControllerMapAction::Start:
        out.buttons |= ButtonStart;
        break;
    case ControllerMapAction::Menu:
        menu_down = true;
        break;
    case ControllerMapAction::LeftStick:
        // Stick-click bit only — axes are never remapped.
        out.buttons |= ButtonLeftStick;
        break;
    case ControllerMapAction::RightStick:
        out.buttons |= ButtonRightStick;
        break;
    case ControllerMapAction::FastForward:
        ff_held = true;
        break;
    case ControllerMapAction::ScreenSwap:
        screen_swap_down = true;
        break;
    }
}

} // namespace

ControllerMapAction ControllerMapProfile::action_for(ControllerMapSource source) const {
    switch (source) {
    case ControllerMapSource::Select:
        return select;
    case ControllerMapSource::Start:
        return start;
    case ControllerMapSource::L:
        return l;
    case ControllerMapSource::R:
        return r;
    case ControllerMapSource::L2:
        return l2;
    case ControllerMapSource::R2:
        return r2;
    case ControllerMapSource::L3:
        return l3;
    case ControllerMapSource::R3:
        return r3;
    }
    return ControllerMapAction::Default;
}

void ControllerMapProfile::set_action(ControllerMapSource source, ControllerMapAction action) {
    switch (source) {
    case ControllerMapSource::Select:
        select = action;
        break;
    case ControllerMapSource::Start:
        start = action;
        break;
    case ControllerMapSource::L:
        l = action;
        break;
    case ControllerMapSource::R:
        r = action;
        break;
    case ControllerMapSource::L2:
        l2 = action;
        break;
    case ControllerMapSource::R2:
        r2 = action;
        break;
    case ControllerMapSource::L3:
        l3 = action;
        break;
    case ControllerMapSource::R3:
        r3 = action;
        break;
    }
}

bool ControllerMapProfile::identity() const {
    return !swap_nw && !swap_se &&
        select == ControllerMapAction::Default &&
        start == ControllerMapAction::Default &&
        l == ControllerMapAction::Default &&
        r == ControllerMapAction::Default &&
        l2 == ControllerMapAction::Default &&
        r2 == ControllerMapAction::Default &&
        l3 == ControllerMapAction::Default &&
        r3 == ControllerMapAction::Default;
}

std::string_view controller_map_family_id(ControllerMapFamily family) {
    switch (family) {
    case ControllerMapFamily::Standard:
        return "standard";
    case ControllerMapFamily::Switch:
        return "switch";
    case ControllerMapFamily::Gba:
        return "gba";
    case ControllerMapFamily::Gb:
        return "gb";
    case ControllerMapFamily::Dual:
        return "dual";
    case ControllerMapFamily::Psx:
        return "psx";
    }
    return "standard";
}

std::string_view controller_map_family_title(ControllerMapFamily family) {
    switch (family) {
    case ControllerMapFamily::Standard:
        return "Standard";
    case ControllerMapFamily::Switch:
        return "Switch";
    case ControllerMapFamily::Gba:
        return "GBA";
    case ControllerMapFamily::Gb:
        return "GB / GBC";
    case ControllerMapFamily::Dual:
        return "DS / 3DS";
    case ControllerMapFamily::Psx:
        return "PSX";
    }
    return "Standard";
}

std::optional<ControllerMapFamily> controller_map_family_from_id(std::string_view id) {
    if (id == "standard") {
        return ControllerMapFamily::Standard;
    }
    if (id == "switch") {
        return ControllerMapFamily::Switch;
    }
    if (id == "gba") {
        return ControllerMapFamily::Gba;
    }
    if (id == "gb") {
        return ControllerMapFamily::Gb;
    }
    if (id == "dual") {
        return ControllerMapFamily::Dual;
    }
    if (id == "psx") {
        return ControllerMapFamily::Psx;
    }
    return std::nullopt;
}

ControllerMapFamily controller_map_family_from_system_key(std::string_view system_key) {
    const auto key = lower_ascii(system_key);
    if (key == "gb" || key == "gbc" || key.find("gameboy") != std::string::npos) {
        return ControllerMapFamily::Gb;
    }
    if (key == "gba" || key.find("gameboyadvance") != std::string::npos) {
        return ControllerMapFamily::Gba;
    }
    if (key == "nds" || key == "3ds" || key == "n3ds" ||
        key.find("nintendo_ds") != std::string::npos ||
        key.find("nintendo_3ds") != std::string::npos) {
        return ControllerMapFamily::Dual;
    }
    if (key == "switch" || key == "nsw" ||
        key.find("nintendo_switch") != std::string::npos ||
        key.find("nintendo switch") != std::string::npos) {
        return ControllerMapFamily::Switch;
    }
    if (key == "ps1" || key == "ps2" || key == "psp" || key == "psx" ||
        key.find("playstation") != std::string::npos) {
        return ControllerMapFamily::Psx;
    }
    return ControllerMapFamily::Standard;
}

std::string_view controller_map_action_id(ControllerMapAction action) {
    switch (action) {
    case ControllerMapAction::Default:
        return "default";
    case ControllerMapAction::A:
        return "a";
    case ControllerMapAction::B:
        return "b";
    case ControllerMapAction::X:
        return "x";
    case ControllerMapAction::Y:
        return "y";
    case ControllerMapAction::L:
        return "l";
    case ControllerMapAction::R:
        return "r";
    case ControllerMapAction::L2:
        return "l2";
    case ControllerMapAction::R2:
        return "r2";
    case ControllerMapAction::Select:
        return "select";
    case ControllerMapAction::Start:
        return "start";
    case ControllerMapAction::Menu:
        return "menu";
    case ControllerMapAction::LeftStick:
        return "left_stick";
    case ControllerMapAction::RightStick:
        return "right_stick";
    case ControllerMapAction::FastForward:
        return "ff";
    case ControllerMapAction::ScreenSwap:
        return "screen_swap";
    }
    return "default";
}

std::string_view controller_map_action_title(ControllerMapAction action) {
    switch (action) {
    case ControllerMapAction::Default:
        return "Default";
    case ControllerMapAction::A:
        return "A";
    case ControllerMapAction::B:
        return "B";
    case ControllerMapAction::X:
        return "X";
    case ControllerMapAction::Y:
        return "Y";
    case ControllerMapAction::L:
        return "L";
    case ControllerMapAction::R:
        return "R";
    case ControllerMapAction::L2:
        return "L2";
    case ControllerMapAction::R2:
        return "R2";
    case ControllerMapAction::Select:
        return "Select";
    case ControllerMapAction::Start:
        return "Start";
    case ControllerMapAction::Menu:
        return "Menu";
    case ControllerMapAction::LeftStick:
        return "L3";
    case ControllerMapAction::RightStick:
        return "R3";
    case ControllerMapAction::FastForward:
        return "Fast-forward";
    case ControllerMapAction::ScreenSwap:
        return "Screen swap";
    }
    return "Default";
}

std::optional<ControllerMapAction> controller_map_action_from_id(std::string_view id) {
    if (id == "default") {
        return ControllerMapAction::Default;
    }
    if (id == "a") {
        return ControllerMapAction::A;
    }
    if (id == "b") {
        return ControllerMapAction::B;
    }
    if (id == "x") {
        return ControllerMapAction::X;
    }
    if (id == "y") {
        return ControllerMapAction::Y;
    }
    if (id == "l") {
        return ControllerMapAction::L;
    }
    if (id == "r") {
        return ControllerMapAction::R;
    }
    if (id == "l2") {
        return ControllerMapAction::L2;
    }
    if (id == "r2") {
        return ControllerMapAction::R2;
    }
    if (id == "select") {
        return ControllerMapAction::Select;
    }
    if (id == "start") {
        return ControllerMapAction::Start;
    }
    if (id == "menu") {
        return ControllerMapAction::Menu;
    }
    if (id == "left_stick") {
        return ControllerMapAction::LeftStick;
    }
    if (id == "right_stick") {
        return ControllerMapAction::RightStick;
    }
    if (id == "ff") {
        return ControllerMapAction::FastForward;
    }
    if (id == "screen_swap") {
        return ControllerMapAction::ScreenSwap;
    }
    return std::nullopt;
}

std::string_view controller_map_source_id(ControllerMapSource source) {
    switch (source) {
    case ControllerMapSource::Select:
        return "select";
    case ControllerMapSource::Start:
        return "start";
    case ControllerMapSource::L:
        return "l";
    case ControllerMapSource::R:
        return "r";
    case ControllerMapSource::L2:
        return "l2";
    case ControllerMapSource::R2:
        return "r2";
    case ControllerMapSource::L3:
        return "l3";
    case ControllerMapSource::R3:
        return "r3";
    }
    return "select";
}

std::string_view controller_map_source_title(ControllerMapSource source) {
    switch (source) {
    case ControllerMapSource::Select:
        return "Select";
    case ControllerMapSource::Start:
        return "Start";
    case ControllerMapSource::L:
        return "L";
    case ControllerMapSource::R:
        return "R";
    case ControllerMapSource::L2:
        return "L2";
    case ControllerMapSource::R2:
        return "R2";
    case ControllerMapSource::L3:
        return "L3";
    case ControllerMapSource::R3:
        return "R3";
    }
    return "Select";
}

std::optional<ControllerMapSource> controller_map_source_from_id(std::string_view id) {
    if (id == "select") {
        return ControllerMapSource::Select;
    }
    if (id == "start") {
        return ControllerMapSource::Start;
    }
    if (id == "l" || id == "shoulder_l") {
        return ControllerMapSource::L;
    }
    if (id == "r" || id == "shoulder_r") {
        return ControllerMapSource::R;
    }
    if (id == "l2" || id == "shoulder_l2") {
        return ControllerMapSource::L2;
    }
    if (id == "r2" || id == "shoulder_r2") {
        return ControllerMapSource::R2;
    }
    if (id == "l3" || id == "left_stick") {
        return ControllerMapSource::L3;
    }
    if (id == "r3" || id == "right_stick") {
        return ControllerMapSource::R3;
    }
    return std::nullopt;
}

ControllerMapAction controller_map_default_action(ControllerMapSource source) {
    switch (source) {
    case ControllerMapSource::Select:
        return ControllerMapAction::Select;
    case ControllerMapSource::Start:
        return ControllerMapAction::Start;
    case ControllerMapSource::L:
        return ControllerMapAction::L;
    case ControllerMapSource::R:
        return ControllerMapAction::R;
    case ControllerMapSource::L2:
        return ControllerMapAction::L2;
    case ControllerMapSource::R2:
        return ControllerMapAction::R2;
    case ControllerMapSource::L3:
        return ControllerMapAction::LeftStick;
    case ControllerMapSource::R3:
        return ControllerMapAction::RightStick;
    }
    return ControllerMapAction::Default;
}

ControllerMapAction controller_map_resolve_action(
    ControllerMapSource source,
    ControllerMapAction stored) {
    if (stored == ControllerMapAction::Default) {
        return controller_map_default_action(source);
    }
    return stored;
}

ControllerState apply_controller_button_map(
    ControllerState state,
    const ControllerMapProfile& profile,
    ControllerMapApplyState& apply_state,
    ControllerMapApplyExtras& extras) {
    extras = {};

    if (profile.identity()) {
        apply_state.prev_menu_down = false;
        apply_state.prev_screen_swap_down = false;
        if (apply_state.prev_ff_held) {
            extras.fast_forward_changed = true;
        }
        apply_state.prev_ff_held = false;
        return state;
    }

    const bool select_down = source_down(state, ControllerMapSource::Select);
    const bool start_down = source_down(state, ControllerMapSource::Start);
    const bool l_down = source_down(state, ControllerMapSource::L);
    const bool r_down = source_down(state, ControllerMapSource::R);
    const bool l2_down = source_down(state, ControllerMapSource::L2);
    const bool r2_down = source_down(state, ControllerMapSource::R2);
    const bool l3_down = source_down(state, ControllerMapSource::L3);
    const bool r3_down = source_down(state, ControllerMapSource::R3);
    const std::uint16_t l2_level = source_analog_level(state, ControllerMapSource::L2);
    const std::uint16_t r2_level = source_analog_level(state, ControllerMapSource::R2);

    auto out = state;
    // Clear remappable digital bits + triggers; stick axes (left_x/y, right_x/y) stay.
    out.buttons &= ~(
        ButtonBack | ButtonStart | ButtonLeftShoulder | ButtonRightShoulder |
        ButtonLeftStick | ButtonRightStick);
    out.left_trigger = 0;
    out.right_trigger = 0;

    bool ff_held = false;
    bool menu_down = false;
    bool screen_swap_down = false;

    const auto dispatch = [&](ControllerMapSource source, bool down, std::uint16_t level) {
        const auto action = controller_map_resolve_action(source, profile.action_for(source));
        apply_action(out, action, down, level, ff_held, menu_down, screen_swap_down);
    };

    dispatch(ControllerMapSource::Select, select_down, 0xFFFF);
    dispatch(ControllerMapSource::Start, start_down, 0xFFFF);
    dispatch(ControllerMapSource::L, l_down, 0xFFFF);
    dispatch(ControllerMapSource::R, r_down, 0xFFFF);
    dispatch(ControllerMapSource::L2, l2_down, l2_level == 0 ? std::uint16_t{0xFFFF} : l2_level);
    dispatch(ControllerMapSource::R2, r2_down, r2_level == 0 ? std::uint16_t{0xFFFF} : r2_level);
    dispatch(ControllerMapSource::L3, l3_down, 0xFFFF);
    dispatch(ControllerMapSource::R3, r3_down, 0xFFFF);

    apply_face_button_swaps(out, profile.swap_nw, profile.swap_se);

    extras.fast_forward_held = ff_held;
    extras.menu_edge = menu_down && !apply_state.prev_menu_down;
    extras.fast_forward_changed = ff_held != apply_state.prev_ff_held;
    extras.screen_swap_edge = screen_swap_down && !apply_state.prev_screen_swap_down;
    apply_state.prev_menu_down = menu_down;
    apply_state.prev_ff_held = ff_held;
    apply_state.prev_screen_swap_down = screen_swap_down;
    return out;
}

namespace {

nlohmann::json profile_to_json(const ControllerMapProfile& profile) {
    return nlohmann::json{
        {"swap_nw", profile.swap_nw},
        {"swap_se", profile.swap_se},
        {"select", controller_map_action_id(profile.select)},
        {"start", controller_map_action_id(profile.start)},
        {"l", controller_map_action_id(profile.l)},
        {"r", controller_map_action_id(profile.r)},
        {"l2", controller_map_action_id(profile.l2)},
        {"r2", controller_map_action_id(profile.r2)},
        {"l3", controller_map_action_id(profile.l3)},
        {"r3", controller_map_action_id(profile.r3)},
    };
}

ControllerMapAction action_from_json_field(const nlohmann::json& object, const char* key) {
    if (!object.contains(key) || !object.at(key).is_string()) {
        return ControllerMapAction::Default;
    }
    return controller_map_action_from_id(object.at(key).get<std::string>())
        .value_or(ControllerMapAction::Default);
}

ControllerMapProfile profile_from_json(const nlohmann::json& object) {
    ControllerMapProfile profile;
    if (!object.is_object()) {
        return profile;
    }
    profile.swap_nw = object.value("swap_nw", false);
    profile.swap_se = object.value("swap_se", false);
    profile.select = action_from_json_field(object, "select");
    profile.start = action_from_json_field(object, "start");
    profile.l = action_from_json_field(object, "l");
    profile.r = action_from_json_field(object, "r");
    profile.l2 = action_from_json_field(object, "l2");
    profile.r2 = action_from_json_field(object, "r2");
    profile.l3 = action_from_json_field(object, "l3");
    profile.r3 = action_from_json_field(object, "r3");
    return profile;
}

} // namespace

std::string controller_map_document_to_json(const ControllerMapDocument& document) {
    nlohmann::json families = nlohmann::json::object();
    for (std::size_t i = 0; i < ControllerMapFamilyCount; ++i) {
        const auto family = static_cast<ControllerMapFamily>(i);
        families[std::string(controller_map_family_id(family))] =
            profile_to_json(document.profiles[i]);
    }
    nlohmann::json root{
        {"version", document.version},
        {"families", std::move(families)},
    };
    return root.dump(2);
}

std::optional<ControllerMapDocument> controller_map_document_from_json(std::string_view json) {
    nlohmann::json root;
    try {
        root = nlohmann::json::parse(json);
    } catch (const nlohmann::json::exception&) {
        return std::nullopt;
    }
    if (!root.is_object()) {
        return std::nullopt;
    }
    ControllerMapDocument document;
    document.version = root.value("version", ControllerMapDocumentVersion);
    const auto& families = root.value("families", nlohmann::json::object());
    if (families.is_object()) {
        for (std::size_t i = 0; i < ControllerMapFamilyCount; ++i) {
            const auto family = static_cast<ControllerMapFamily>(i);
            const auto id = std::string(controller_map_family_id(family));
            if (families.contains(id)) {
                document.profiles[i] = profile_from_json(families.at(id));
            }
        }
    }
    return document;
}

bool controller_map_document_save_file(
    const std::filesystem::path& path,
    const ControllerMapDocument& document) {
    std::error_code ec;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), ec);
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    out << controller_map_document_to_json(document);
    return static_cast<bool>(out);
}

std::optional<ControllerMapDocument> controller_map_document_load_file(
    const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return controller_map_document_from_json(buffer.str());
}

} // namespace archstreamer
