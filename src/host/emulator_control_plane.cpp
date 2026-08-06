#include "host/emulator_control_plane.hpp"

#include "host/virtual_keyboard.hpp"

#include <iostream>

namespace archstreamer {

EmulatorControlPlane::EmulatorControlPlane(VirtualKeyboard* keyboard)
    : keyboard_(keyboard) {}

void EmulatorControlPlane::set_keyboard(VirtualKeyboard* keyboard) {
    keyboard_ = keyboard;
}

void EmulatorControlPlane::set_backend(EmulatorControlBackend backend) {
    backend_ = backend;
    if (keyboard_ == nullptr) {
        return;
    }
    // Ryujinx: F5 pause + F1 VSync FF. melonDS: F5 pause + Space hold-FF.
    // RetroArch: netcmd pause + Space hold-FF.
    keyboard_->set_switch_style_hotkeys(backend == EmulatorControlBackend::Ryujinx);
    keyboard_->set_melonds_style_hotkeys(backend == EmulatorControlBackend::MelonDS);
}

void EmulatorControlPlane::apply_from_client(const EmulatorControl& control) {
    if (keyboard_ == nullptr || !keyboard_->plugged()) {
        return;
    }
    const bool force = control.force != 0;
    if (control.pause != EmulatorControlState::Unchanged) {
        apply_stateful(
            EmulatorIntentKind::Pause,
            control.pause == EmulatorControlState::On,
            force);
    }
    if (control.fast_forward != EmulatorControlState::Unchanged) {
        apply_stateful(
            EmulatorIntentKind::FastForward,
            control.fast_forward == EmulatorControlState::On,
            force);
    } else if (control.pause == EmulatorControlState::Off) {
        // Unpause can steal focus; re-assert hold-FF when still desired (RA Space / Ryu F6).
        keyboard_->reassert_fast_forward_hold();
    }
    if (control.action != EmulatorControlActionNone) {
        apply_action(static_cast<EmulatorIntentKind>(control.action));
    }
}

void EmulatorControlPlane::apply_stateful(
    EmulatorIntentKind kind,
    bool want_on,
    bool force) {
    if (keyboard_ == nullptr) {
        return;
    }
    switch (kind) {
    case EmulatorIntentKind::Pause:
        keyboard_->set_paused(want_on, force);
        break;
    case EmulatorIntentKind::FastForward:
        keyboard_->set_fast_forward(want_on, force);
        break;
    default:
        std::cerr
            << "EmulatorControlPlane: unhandled stateful intent "
            << static_cast<int>(kind) << '\n';
        break;
    }
}

void EmulatorControlPlane::apply_action(EmulatorIntentKind kind) {
    if (keyboard_ == nullptr) {
        return;
    }
    switch (kind) {
    case EmulatorIntentKind::ScreenSwap:
        keyboard_->trigger_screen_swap();
        break;
    default:
        std::cerr
            << "EmulatorControlPlane: action intent "
            << static_cast<int>(kind)
            << " not implemented\n";
        break;
    }
}

} // namespace archstreamer
