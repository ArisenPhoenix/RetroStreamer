#include "host/virtual_keyboard.hpp"

#include "host/retroarch_netcmd.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <X11/extensions/XTest.h>

namespace archstreamer {
namespace {

constexpr RemotedKeyBinding kDefaultBindings[] = {
    // Prefer XTest Space over FAST_FORWARD netcmd: input_hold_fast_forward=space in the
    // session cfg, and toggle-netcmd desyncs easily (pause works; FF often looks dead).
    {KeySpace, RemotedKeyAction::XTestHold, nullptr},
    {KeyP, RemotedKeyAction::NetcmdPress, "PAUSE_TOGGLE"},
    {KeyF1, RemotedKeyAction::NetcmdPress, "MENU_TOGGLE"},
    {KeyUp, RemotedKeyAction::XTestHold, nullptr},
    {KeyDown, RemotedKeyAction::XTestHold, nullptr},
    {KeyLeft, RemotedKeyAction::XTestHold, nullptr},
    {KeyRight, RemotedKeyAction::XTestHold, nullptr},
    {KeyEnter, RemotedKeyAction::XTestHold, nullptr},
    {KeyEscape, RemotedKeyAction::XTestHold, nullptr},
    {KeyTab, RemotedKeyAction::XTestHold, nullptr},
    {KeyBackspace, RemotedKeyAction::XTestHold, nullptr},
};

KeySym xtest_keysym(RemotedKey key) {
    switch (key) {
    case KeySpace:
        return XK_space;
    case KeyUp:
        return XK_Up;
    case KeyDown:
        return XK_Down;
    case KeyLeft:
        return XK_Left;
    case KeyRight:
        return XK_Right;
    case KeyEnter:
        return XK_Return;
    case KeyEscape:
        return XK_Escape;
    case KeyTab:
        return XK_Tab;
    case KeyBackspace:
        return XK_BackSpace;
    default:
        return NoSymbol;
    }
}

Display* as_display(void* ptr) {
    return static_cast<Display*>(ptr);
}

bool bit_down(std::uint32_t keys, RemotedKey key) {
    return (keys & static_cast<std::uint32_t>(key)) != 0;
}

} // namespace

const RemotedKeyBinding* default_remoted_key_bindings(std::size_t& count) {
    count = sizeof(kDefaultBindings) / sizeof(kDefaultBindings[0]);
    return kDefaultBindings;
}

VirtualKeyboard::VirtualKeyboard(std::string capture_display)
    : capture_display_(std::move(capture_display)) {
}

VirtualKeyboard::~VirtualKeyboard() {
    unplug();
}

void VirtualKeyboard::ensure_xtest_display() {
    if (display_ != nullptr || capture_display_.empty()) {
        return;
    }
    Display* display = XOpenDisplay(capture_display_.c_str());
    if (display == nullptr) {
        throw std::runtime_error("failed to open X display " + capture_display_ + " for virtual keyboard");
    }
    int event_base = 0;
    int error_base = 0;
    int major = 0;
    int minor = 0;
    if (!XTestQueryExtension(display, &event_base, &error_base, &major, &minor)) {
        XCloseDisplay(display);
        throw std::runtime_error("XTest extension missing on display " + capture_display_);
    }
    display_ = display;
}

void VirtualKeyboard::plug() {
    if (plugged_) {
        return;
    }
    if (capture_display_.empty()) {
        throw std::runtime_error("virtual keyboard requires the RetroArch X display (e.g. :99)");
    }
    ensure_xtest_display();
    plugged_ = true;
    has_last_ = false;
    last_keys_ = 0;
    std::cout
        << "Virtual keyboard ready on " << capture_display_
        << " (Space→XTest hold-FF, P→pause, F1→menu; arrows/Enter/Esc→XTest)\n";
}

void VirtualKeyboard::unplug() {
    if (!plugged_ && display_ == nullptr) {
        return;
    }
    if (plugged_) {
        try {
            release_all();
        } catch (...) {
        }
    }
    if (display_ != nullptr) {
        XCloseDisplay(as_display(display_));
        display_ = nullptr;
    }
    plugged_ = false;
    has_last_ = false;
    last_keys_ = 0;
}

void VirtualKeyboard::apply_xtest_edges(std::uint32_t previous, std::uint32_t next) {
    if (display_ == nullptr) {
        return;
    }
    Display* display = as_display(display_);
    bool any = false;
    std::size_t count = 0;
    const auto* bindings = default_remoted_key_bindings(count);
    for (std::size_t i = 0; i < count; ++i) {
        const auto& binding = bindings[i];
        if (binding.action != RemotedKeyAction::XTestHold) {
            continue;
        }
        const bool was_down = bit_down(previous, binding.key);
        const bool is_down = bit_down(next, binding.key);
        if (was_down == is_down) {
            continue;
        }
        const KeySym sym = xtest_keysym(binding.key);
        if (sym == NoSymbol) {
            continue;
        }
        const KeyCode code = XKeysymToKeycode(display, sym);
        if (code == 0) {
            continue;
        }
        XTestFakeKeyEvent(display, code, is_down ? True : False, CurrentTime);
        any = true;
    }
    if (any) {
        XFlush(display);
    }
}

void VirtualKeyboard::apply(const KeyboardState& state) {
    if (!plugged_) {
        return;
    }

    const std::uint32_t previous = has_last_ ? last_keys_ : 0;
    const std::uint32_t next = state.keys;

    std::size_t count = 0;
    const auto* bindings = default_remoted_key_bindings(count);
    for (std::size_t i = 0; i < count; ++i) {
        const auto& binding = bindings[i];
        const bool was_down = bit_down(previous, binding.key);
        const bool is_down = bit_down(next, binding.key);
        if (was_down == is_down) {
            continue;
        }

        switch (binding.action) {
        case RemotedKeyAction::NetcmdEdgeToggle:
            if (binding.netcmd != nullptr) {
                send_retroarch_netcmd(binding.netcmd);
            }
            break;
        case RemotedKeyAction::NetcmdPress:
            if (is_down && binding.netcmd != nullptr) {
                if (send_retroarch_netcmd(binding.netcmd)) {
                    std::cout
                        << "Keyboard netcmd: "
                        << (binding.key == KeyP ? "P" : binding.key == KeyF1 ? "F1" : "?")
                        << " → " << binding.netcmd << '\n';
                }
            }
            break;
        case RemotedKeyAction::XTestHold:
            if (binding.key == KeySpace && is_down && !logged_ff_) {
                logged_ff_ = true;
                std::cout << "Fast-forward: Space held via XTest (input_hold_fast_forward)\n";
            }
            break;
        case RemotedKeyAction::Ignored:
            break;
        }
    }

    if (previous != next) {
        apply_xtest_edges(previous, next);
    }

    last_keys_ = next;
    has_last_ = true;
}

void VirtualKeyboard::release_all() {
    KeyboardState empty{};
    apply(empty);
}

} // namespace archstreamer
