#ifndef _WIN32
#include "host/virtual_keyboard.hpp"

#include "host/retroarch_netcmd.hpp"

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <X11/Xlib.h>
#include <X11/Xatom.h>
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
    // Xlib's default fatal I/O path calls exit(1). When a session slot stops Xvfb,
    // that would kill the whole host_runner lobby. Prefer closing the Display and
    // continuing so concurrent sessions / the accept loop stay alive.
    XSetIOErrorExitHandler(
        display,
        [](Display*, void*) {
            // Intentionally empty: do not terminate the process.
        },
        nullptr);
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
        // XCloseDisplay can itself trip the fatal I/O path if Xvfb is already gone;
        // the IO exit handler above keeps host_runner alive.
        try {
            XCloseDisplay(as_display(display_));
        } catch (...) {
        }
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

namespace {

std::string window_title(Display* display, Window window) {
    Atom actual_type = None;
    int actual_format = 0;
    unsigned long item_count = 0;
    unsigned long bytes_after = 0;
    unsigned char* prop = nullptr;
    if (XGetWindowProperty(
            display,
            window,
            XInternAtom(display, "_NET_WM_NAME", False),
            0,
            256,
            False,
            AnyPropertyType,
            &actual_type,
            &actual_format,
            &item_count,
            &bytes_after,
            &prop) == Success &&
        prop != nullptr) {
        std::string name(reinterpret_cast<char*>(prop));
        XFree(prop);
        return name;
    }
    char* legacy = nullptr;
    if (XFetchName(display, window, &legacy) && legacy != nullptr) {
        std::string name(legacy);
        XFree(legacy);
        return name;
    }
    return {};
}

void collect_windows(Display* display, Window window, std::vector<std::pair<Window, std::string>>& out) {
    Window root = 0;
    Window parent = 0;
    Window* children = nullptr;
    unsigned int child_count = 0;
    if (!XQueryTree(display, window, &root, &parent, &children, &child_count) ||
        children == nullptr) {
        return;
    }
    for (unsigned int index = 0; index < child_count; ++index) {
        const auto title = window_title(display, children[index]);
        if (!title.empty()) {
            out.emplace_back(children[index], title);
        }
        collect_windows(display, children[index], out);
    }
    XFree(children);
}

void xtest_key(Display* display, KeySym keysym, bool pressed) {
    const KeyCode code = XKeysymToKeycode(display, keysym);
    if (code == 0) {
        return;
    }
    XTestFakeKeyEvent(display, code, pressed ? True : False, CurrentTime);
    XFlush(display);
}

void xtest_type_ascii(Display* display, const std::string& text) {
    for (char character : text) {
        KeySym keysym = NoSymbol;
        bool shift = false;
        if (character >= 'A' && character <= 'Z') {
            shift = true;
            keysym = XStringToKeysym(std::string(1, static_cast<char>(character - 'A' + 'a')).c_str());
        } else if (character >= 'a' && character <= 'z') {
            keysym = XStringToKeysym(std::string(1, character).c_str());
        } else if (character >= '0' && character <= '9') {
            keysym = XStringToKeysym(std::string(1, character).c_str());
        } else if (character == ' ' || character == '_' || character == '-') {
            keysym = character == ' ' ? XK_space : XStringToKeysym(std::string(1, character).c_str());
        }
        if (keysym == NoSymbol) {
            continue;
        }
        if (shift) {
            xtest_key(display, XK_Shift_L, true);
        }
        xtest_key(display, keysym, true);
        xtest_key(display, keysym, false);
        if (shift) {
            xtest_key(display, XK_Shift_L, false);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
    }
}

bool try_autofill_on_display(const std::string& display_name, const std::string& text) {
    Display* display = XOpenDisplay(display_name.c_str());
    if (display == nullptr) {
        return false;
    }
    XSetIOErrorExitHandler(
        display,
        [](Display*, void*) {},
        nullptr);

    std::vector<std::pair<Window, std::string>> windows;
    collect_windows(display, DefaultRootWindow(display), windows);

    Window target = 0;
    for (const auto& [window, title] : windows) {
        if (title.find("ContentDialogOverlayWindow") != std::string::npos ||
            title.find("Software Keyboard") != std::string::npos) {
            target = window;
            break;
        }
    }
    if (target == 0) {
        XCloseDisplay(display);
        return false;
    }

    XSetInputFocus(display, target, RevertToParent, CurrentTime);
    XRaiseWindow(display, target);
    XFlush(display);
    // Give Avalonia a beat to focus the text box before the first glyph.
    std::this_thread::sleep_for(std::chrono::milliseconds(250));

    xtest_type_ascii(display, text);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    xtest_key(display, XK_Return, true);
    xtest_key(display, XK_Return, false);
    XCloseDisplay(display);
    std::cout
        << "Ryujinx Software Keyboard: autofilled \"" << text
        << "\" on display " << display_name << '\n';
    return true;
}

} // namespace

void schedule_ryujinx_name_dialog_autofill(std::string text) {
    // Pokemon trainer names are 1–12 Latin characters.
    if (text.size() > 12) {
        text.resize(12);
    }
    if (text.empty()) {
        text = "Player";
    } else {
        // Match Profiles.json title-case (e.g. "alina" → "Alina").
        bool capitalize = true;
        for (char& character : text) {
            if (character == ' ' || character == '-' || character == '_') {
                if (character == '_' || character == '-') {
                    character = ' ';
                }
                capitalize = true;
                continue;
            }
            if (capitalize && character >= 'a' && character <= 'z') {
                character = static_cast<char>(character - 'a' + 'A');
            } else if (!capitalize && character >= 'A' && character <= 'Z') {
                character = static_cast<char>(character - 'A' + 'a');
            }
            capitalize = false;
        }
    }
    std::thread([text = std::move(text)]() {
        // Dialog appears well after boot (shader/cache); poll for ~3 minutes.
        for (int attempt = 0; attempt < 360; ++attempt) {
            for (int display_index = 1; display_index <= 10; ++display_index) {
                const auto name = ":" + std::to_string(display_index);
                try {
                    if (try_autofill_on_display(name, text)) {
                        return;
                    }
                } catch (...) {
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        std::cerr << "Ryujinx Software Keyboard: autofill timed out waiting for dialog\n";
    }).detach();
}

} // namespace archstreamer

#else

#include "host/virtual_keyboard.hpp"

namespace archstreamer {

const RemotedKeyBinding* default_remoted_key_bindings(std::size_t& count) {
    count = 0;
    return nullptr;
}

VirtualKeyboard::VirtualKeyboard(std::string) {}
VirtualKeyboard::~VirtualKeyboard() = default;
void VirtualKeyboard::plug() {}
void VirtualKeyboard::unplug() {}
void VirtualKeyboard::apply(const KeyboardState&) {}
void VirtualKeyboard::release_all() {}

} // namespace archstreamer

#endif
