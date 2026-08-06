#pragma once

#include "common/controller_state.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

/**
 * Placement-free physical-controller remaps shared by desktop and mobile.
 *
 * Portable JSON document (controller_button_map.json, see shared/):
 * version + families.<id> with swap_nw, swap_se, select, start, l, r, l2, r2, l3, r3.
 * Action / family / source ids match across C++ and Kotlin. Face swaps live here too
 * so one profile owns the full client-side pad transform before ControllerInput hits
 * the wire. Host stays a dumb relay.
 */
namespace archstreamer {

/** Per-system remap family (aligned with mobile overlay families + PSX). */
enum class ControllerMapFamily : std::uint8_t {
    Standard = 0,
    Switch = 1,
    Gba = 2,
    Gb = 3,
    Dual = 4,
    /** PS1 / PS2 / PSP — one profile for all PlayStation kinds. */
    Psx = 5,
};

inline constexpr std::size_t ControllerMapFamilyCount = 6;

/**
 * What a remappable physical control does when pressed.
 * Ids match mobile OverlayAction.
 */
enum class ControllerMapAction : std::uint8_t {
    Default = 0,
    A,
    B,
    X,
    Y,
    L,
    R,
    L2,
    R2,
    Select,
    Start,
    Menu,
    LeftStick,
    RightStick,
    FastForward,
    ScreenSwap,
};

/**
 * Physical inputs that accept Action remaps.
 * Stick *clicks* (L3/R3) are remappable; stick axes / face / d-pad stay fixed.
 */
enum class ControllerMapSource : std::uint8_t {
    Select = 0,
    Start,
    L,
    R,
    L2,
    R2,
    L3,
    R3,
};

inline constexpr std::size_t ControllerMapSourceCount = 8;

struct ControllerMapProfile {
    bool swap_nw = false;
    bool swap_se = false;
    ControllerMapAction select = ControllerMapAction::Default;
    ControllerMapAction start = ControllerMapAction::Default;
    ControllerMapAction l = ControllerMapAction::Default;
    ControllerMapAction r = ControllerMapAction::Default;
    ControllerMapAction l2 = ControllerMapAction::Default;
    ControllerMapAction r2 = ControllerMapAction::Default;
    ControllerMapAction l3 = ControllerMapAction::Default;
    ControllerMapAction r3 = ControllerMapAction::Default;

    ControllerMapAction action_for(ControllerMapSource source) const;
    void set_action(ControllerMapSource source, ControllerMapAction action);

    bool identity() const;
};

/** Rising-edge / hold bookkeeping for Menu, Fast-forward, and Screen-swap side effects. */
struct ControllerMapApplyState {
    bool prev_menu_down = false;
    bool prev_ff_held = false;
    bool prev_screen_swap_down = false;
};

struct ControllerMapApplyExtras {
    bool fast_forward_held = false;
    /** True on the frame a Menu-bound source is newly pressed. */
    bool menu_edge = false;
    bool fast_forward_changed = false;
    /** True on the frame a ScreenSwap-bound source is newly pressed. */
    bool screen_swap_edge = false;
};

std::string_view controller_map_family_id(ControllerMapFamily family);
std::string_view controller_map_family_title(ControllerMapFamily family);
std::optional<ControllerMapFamily> controller_map_family_from_id(std::string_view id);
ControllerMapFamily controller_map_family_from_system_key(std::string_view system_key);

std::string_view controller_map_action_id(ControllerMapAction action);
std::string_view controller_map_action_title(ControllerMapAction action);
std::optional<ControllerMapAction> controller_map_action_from_id(std::string_view id);

std::string_view controller_map_source_id(ControllerMapSource source);
std::string_view controller_map_source_title(ControllerMapSource source);
std::optional<ControllerMapSource> controller_map_source_from_id(std::string_view id);

/** Default (natural) action for a physical source when profile stores Default. */
ControllerMapAction controller_map_default_action(ControllerMapSource source);
ControllerMapAction controller_map_resolve_action(
    ControllerMapSource source,
    ControllerMapAction stored);

/**
 * Remap Select/Start/L/R/L2/R2/L3/R3, then apply face swaps.
 * Stick axes are never remapped — only the click bits. Trigger "down" uses a
 * small analog threshold (matches mobile physical tracker).
 */
ControllerState apply_controller_button_map(
    ControllerState state,
    const ControllerMapProfile& profile,
    ControllerMapApplyState& apply_state,
    ControllerMapApplyExtras& extras);

/** Portable on-disk document (desktop + mobile). Filename is fixed across platforms. */
inline constexpr std::string_view ControllerMapFileName = "controller_button_map.json";
inline constexpr int ControllerMapDocumentVersion = 1;

struct ControllerMapDocument {
    int version = ControllerMapDocumentVersion;
    std::array<ControllerMapProfile, ControllerMapFamilyCount> profiles{};

    ControllerMapProfile& profile(ControllerMapFamily family) {
        return profiles[static_cast<std::size_t>(family)];
    }

    const ControllerMapProfile& profile(ControllerMapFamily family) const {
        return profiles[static_cast<std::size_t>(family)];
    }
};

/** JSON object with version + families.<id>.{swap_nw,swap_se,select,…}. */
std::string controller_map_document_to_json(const ControllerMapDocument& document);
std::optional<ControllerMapDocument> controller_map_document_from_json(std::string_view json);

bool controller_map_document_save_file(
    const std::filesystem::path& path,
    const ControllerMapDocument& document);
std::optional<ControllerMapDocument> controller_map_document_load_file(
    const std::filesystem::path& path);

} // namespace archstreamer
