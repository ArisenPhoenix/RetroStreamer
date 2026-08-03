#ifndef _WIN32
#include "host/virtual_keyboard.hpp"
#include "host/soft_keyboard_host.hpp"

#include "host/retroarch_netcmd.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
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
    // Desktop Space: XTest hold for RetroArch hold-FF; Switch uses edge taps in apply().
    {KeySpace, RemotedKeyAction::XTestHold, nullptr},
    // Desktop P: one-shot pause toggle (explicit set via EmulatorControl preferred).
    {KeyP, RemotedKeyAction::NetcmdPress, nullptr},
    {KeyF1, RemotedKeyAction::NetcmdPress, "MENU_TOGGLE"},
    // Yuzu: Toggle Framerate Limit (continuous uncapped speed). Bound in qt-config.
    {KeyF8, RemotedKeyAction::XTestHold, nullptr},
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
    case KeyF8:
        return XK_F8;
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

void install_x_io_exit_guard(Display* display);

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

void VirtualKeyboard::rebind_display(std::string capture_display) {
    unplug();
    capture_display_ = std::move(capture_display);
}

void VirtualKeyboard::ensure_xtest_display() {
    if (display_ != nullptr || capture_display_.empty()) {
        return;
    }
    Display* display = XOpenDisplay(capture_display_.c_str());
    if (display == nullptr) {
        throw std::runtime_error("failed to open X display " + capture_display_ + " for virtual keyboard");
    }
    // Xlib calls exit(1) after any IOExitHandler *returns*. Never return.
    install_x_io_exit_guard(display);
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
    paused_ = false;
    fast_forward_ = false;
    ff_space_held_ = false;
    std::cout
        << "Virtual keyboard ready on " << capture_display_
        << (switch_style_hotkeys_
                ? " (Ryujinx FF→hold F6 turbo@200%, P→F5 pause; arrows/Enter/Esc→XTest)\n"
                : " (Space→XTest hold-FF, F8→Yuzu continuous FF, P→pause, F1→menu; arrows/Enter/Esc→XTest)\n");
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
        // Never XCloseDisplay here. If gamescope/Xvfb already tore down the socket,
        // Close triggers XIO → exit(1) even with an IOExitHandler that returns.
        // Leak the Display*; the OS reclaims the fd when host_runner exits.
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
    bool need_focus = false;
    std::size_t count = 0;
    const auto* bindings = default_remoted_key_bindings(count);
    for (std::size_t i = 0; i < count; ++i) {
        const auto& binding = bindings[i];
        if (binding.action != RemotedKeyAction::XTestHold) {
            continue;
        }
        // Space hold/release is handled in apply() (focus + refresh while held).
        if (binding.key == KeySpace) {
            continue;
        }
        const bool was_down = bit_down(previous, binding.key);
        const bool is_down = bit_down(next, binding.key);
        if (was_down == is_down) {
            continue;
        }
        need_focus = true;
        const KeySym sym = xtest_keysym(binding.key);
        if (sym == NoSymbol) {
            continue;
        }
        const KeyCode code = XKeysymToKeycode(display, sym);
        if (code == 0) {
            continue;
        }
        if (need_focus) {
            focus_emulator_window(false);
            need_focus = false;
        }
        XTestFakeKeyEvent(display, code, is_down ? True : False, CurrentTime);
        any = true;
    }
    if (any) {
        XFlush(display);
    }
}

void VirtualKeyboard::apply_xtest_space_autorepeat() {
    // Unused — Space is handled in apply() / set_retroarch_ff_space_held().
}

void VirtualKeyboard::xtest_tap_keysym(unsigned long keysym) {
    ensure_xtest_display();
    if (display_ == nullptr) {
        return;
    }
    Display* display = as_display(display_);
    const KeyCode code = XKeysymToKeycode(display, static_cast<KeySym>(keysym));
    if (code == 0) {
        return;
    }
    focus_emulator_window(/*settle=*/true);
    XTestFakeKeyEvent(display, code, True, CurrentTime);
    XFlush(display);
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    XTestFakeKeyEvent(display, code, False, CurrentTime);
    XFlush(display);
}

void VirtualKeyboard::xtest_set_keysym(unsigned long keysym, bool down) {
    ensure_xtest_display();
    if (display_ == nullptr) {
        return;
    }
    Display* display = as_display(display_);
    const KeyCode code = XKeysymToKeycode(display, static_cast<KeySym>(keysym));
    if (code == 0) {
        return;
    }
    focus_emulator_window(/*settle=*/true);
    XTestFakeKeyEvent(display, code, down ? True : False, CurrentTime);
    XFlush(display);
}

void VirtualKeyboard::set_retroarch_ff_space_held(bool want_held) {
    if (want_held == ff_space_held_) {
        if (want_held) {
            // Re-assert focus + down in case the capture window ate the key.
            xtest_set_keysym(XK_space, true);
        }
        return;
    }
    xtest_set_keysym(XK_space, want_held);
    ff_space_held_ = want_held;
}

namespace {

// Xlib's default protocol-error handler prints to stderr and calls exit(1). We probe
// windows owned by the emulator, and those can be destroyed between the XQueryTree that
// hands us the id and the property fetch that follows, so BadWindow is routine here.
// Without this the host dies mid-session on a race it should just retry past.
void install_x_error_guard() {
    static std::once_flag once;
    std::call_once(once, [] {
        XSetErrorHandler([](Display*, XErrorEvent*) { return 0; });
    });
}

/** Xlib calls exit(1) if an IOExitHandler returns. Park the thread instead. */
void install_x_io_exit_guard(Display* display) {
    if (display == nullptr) {
        return;
    }
    XSetIOErrorExitHandler(
        display,
        [](Display*, void*) {
            for (;;) {
                std::this_thread::sleep_for(std::chrono::hours(24));
            }
        },
        nullptr);
}

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

bool window_is_viewable(Display* display, Window window) {
    XWindowAttributes attrs{};
    if (!XGetWindowAttributes(display, window, &attrs)) {
        return false;
    }
    return attrs.map_state == IsViewable && attrs.width >= 64 && attrs.height >= 64;
}

bool title_looks_like_emulator(const std::string& title) {
    auto lower = title;
    for (char& character : lower) {
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }
    return lower.find("ryujinx") != std::string::npos ||
        lower.find("yuzu") != std::string::npos ||
        lower.find("suyu") != std::string::npos ||
        lower.find("sudachi") != std::string::npos ||
        lower.find("retroarch") != std::string::npos;
}

std::optional<int> window_pid(Display* display, Window window) {
    Atom actual_type = None;
    int actual_format = 0;
    unsigned long item_count = 0;
    unsigned long bytes_after = 0;
    unsigned char* prop = nullptr;
    if (XGetWindowProperty(
            display,
            window,
            XInternAtom(display, "_NET_WM_PID", False),
            0,
            1,
            False,
            XA_CARDINAL,
            &actual_type,
            &actual_format,
            &item_count,
            &bytes_after,
            &prop) != Success ||
        prop == nullptr ||
        item_count < 1) {
        if (prop != nullptr) {
            XFree(prop);
        }
        return std::nullopt;
    }
    const auto pid = static_cast<int>(*reinterpret_cast<unsigned long*>(prop));
    XFree(prop);
    return pid > 0 ? std::optional<int>{pid} : std::nullopt;
}

bool pid_in_process_tree(int candidate_pid, int root_pid) {
    if (root_pid <= 0 || candidate_pid <= 0) {
        return false;
    }
    if (candidate_pid == root_pid) {
        return true;
    }
    // Walk candidate's parents toward init — covers AppImage wrappers under gamescope.
    int current = candidate_pid;
    for (int depth = 0; depth < 64 && current > 1; ++depth) {
        std::ifstream in("/proc/" + std::to_string(current) + "/stat");
        std::string ignore;
        int pid = 0;
        char state = 0;
        int ppid = 0;
        if (!(in >> pid >> ignore >> state >> ppid)) {
            break;
        }
        if (ppid == root_pid) {
            return true;
        }
        if (ppid <= 1 || ppid == current) {
            break;
        }
        current = ppid;
    }
    // Also: candidate may be an ancestor of root (gamescope leader vs nested Ryujinx).
    current = root_pid;
    for (int depth = 0; depth < 64 && current > 1; ++depth) {
        if (current == candidate_pid) {
            return true;
        }
        std::ifstream in("/proc/" + std::to_string(current) + "/stat");
        std::string ignore;
        int pid = 0;
        char state = 0;
        int ppid = 0;
        if (!(in >> pid >> ignore >> state >> ppid)) {
            break;
        }
        if (ppid <= 1 || ppid == current) {
            break;
        }
        current = ppid;
    }
    return false;
}

void activate_x_window(Display* display, Window window) {
    // Match raise_video_window: _NET_ACTIVE_WINDOW is what gamescope/steamcompmgr honor.
    XEvent event{};
    event.xclient.type = ClientMessage;
    event.xclient.window = window;
    event.xclient.message_type = XInternAtom(display, "_NET_ACTIVE_WINDOW", False);
    event.xclient.format = 32;
    event.xclient.data.l[0] = 2;
    event.xclient.data.l[1] = static_cast<long>(CurrentTime);
    XSendEvent(
        display,
        DefaultRootWindow(display),
        False,
        SubstructureRedirectMask | SubstructureNotifyMask,
        &event);
    XRaiseWindow(display, window);
    XSetInputFocus(display, window, RevertToParent, CurrentTime);
    XFlush(display);
}

Window find_emulator_window(Display* display, int target_pid) {
    std::vector<std::pair<Window, std::string>> windows;
    collect_windows(display, DefaultRootWindow(display), windows);

    Window best = None;
    int best_area = 0;
    for (const auto& [window, title] : windows) {
        if (!window_is_viewable(display, window)) {
            continue;
        }
        bool match = false;
        if (target_pid > 0) {
            if (const auto pid = window_pid(display, window); pid && pid_in_process_tree(*pid, target_pid)) {
                match = true;
            }
        }
        if (!match && title_looks_like_emulator(title)) {
            match = true;
        }
        // Never treat gamescope's compositor shell as the game.
        auto lower = title;
        for (char& character : lower) {
            character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
        }
        if (lower.find("steamcompmgr") != std::string::npos) {
            continue;
        }
        if (!match) {
            continue;
        }
        XWindowAttributes attrs{};
        if (!XGetWindowAttributes(display, window, &attrs)) {
            continue;
        }
        const int area = attrs.width * attrs.height;
        if (area > best_area) {
            best_area = area;
            best = window;
        }
    }
    return best;
}

bool is_soft_keyboard_dialog_title(const std::string& title) {
    // Avalonia hosts swkbd in ContentDialogOverlayWindow (Title is hard-coded in
    // Ryujinx XAML). GTK builds use a real "Software Keyboard" window title.
    return title.find("Software Keyboard") != std::string::npos ||
        title.find("ContentDialogOverlayWindow") != std::string::npos;
}

// A dialog that has opened and is accepting typed input: viewable + keyboard focus.
//
// Walks up from the focused window rather than enumerating the whole tree. A desktop
// display holds hundreds of windows and each one costs a title round-trip, so the
// enumerating version took seconds per tick and the pad OSK showed up late.
std::optional<Window> find_focused_text_dialog(Display* display) {
    Window focus = 0;
    int revert = RevertToNone;
    XGetInputFocus(display, &focus, &revert);
    if (focus == 0 || focus == None || focus == PointerRoot) {
        return std::nullopt;
    }

    Window current = focus;
    for (int depth = 0; depth < 64; ++depth) {
        const auto title = window_title(display, current);
        if (is_soft_keyboard_dialog_title(title) && window_is_viewable(display, current)) {
            return current;
        }
        Window root = 0;
        Window parent = 0;
        Window* children = nullptr;
        unsigned int child_count = 0;
        if (!XQueryTree(display, current, &root, &parent, &children, &child_count)) {
            return std::nullopt;
        }
        if (children != nullptr) {
            XFree(children);
        }
        if (parent == 0 || parent == current || parent == root) {
            return std::nullopt;
        }
        current = parent;
    }
    return std::nullopt;
}

enum class TextDialogProbe {
    // Display could not be opened at all (candidate slot is not in use).
    Unavailable,
    NoDialog,
    Ready,
};

TextDialogProbe probe_text_dialog(const std::string& display_name) {
    install_x_error_guard();
    Display* display = XOpenDisplay(display_name.c_str());
    if (display == nullptr) {
        return TextDialogProbe::Unavailable;
    }
    install_x_io_exit_guard(display);

    const bool ready = find_focused_text_dialog(display).has_value();
    XCloseDisplay(display);
    return ready ? TextDialogProbe::Ready : TextDialogProbe::NoDialog;
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

bool try_autofill_on_display(
    const std::string& display_name,
    const std::string& text,
    bool allow_any_focused = false) {
    install_x_error_guard();
    Display* display = XOpenDisplay(display_name.c_str());
    if (display == nullptr) {
        return false;
    }
    install_x_io_exit_guard(display);

    Window target = 0;
    if (const auto focused = find_focused_text_dialog(display); focused.has_value()) {
        target = *focused;
    } else {
        // Dialog may still be up but focus briefly moved; fall back to any viewable match.
        std::vector<std::pair<Window, std::string>> windows;
        collect_windows(display, DefaultRootWindow(display), windows);
        for (const auto& [window, title] : windows) {
            if (is_soft_keyboard_dialog_title(title) && window_is_viewable(display, window)) {
                target = window;
                break;
            }
        }
    }
    // Manual pad-OSK escape hatch: second prompts (e.g. Pokemon nickname confirmations)
    // sometimes use a different Avalonia title than the ones we auto-detect.
    if (target == 0 && allow_any_focused) {
        Window focus = 0;
        int revert = RevertToNone;
        XGetInputFocus(display, &focus, &revert);
        if (focus != 0 && focus != None && focus != PointerRoot &&
            window_is_viewable(display, focus)) {
            target = focus;
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

    // Ryujinx seeds swkbd with the current Switch profile nickname, so typing alone
    // appends to it and the player ends up with "AlinaAlina".
    xtest_key(display, XK_Control_L, true);
    xtest_key(display, XK_a, true);
    xtest_key(display, XK_a, false);
    xtest_key(display, XK_Control_L, false);
    xtest_key(display, XK_BackSpace, true);
    xtest_key(display, XK_BackSpace, false);
    std::this_thread::sleep_for(std::chrono::milliseconds(80));

    xtest_type_ascii(display, text);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    xtest_key(display, XK_Return, true);
    xtest_key(display, XK_Return, false);
    XCloseDisplay(display);
    std::cout
        << "Ryujinx Software Keyboard: injected \"" << text
        << "\" on display " << display_name << '\n';
    return true;
}

} // namespace

bool VirtualKeyboard::focus_emulator_window(bool settle) {
    if (display_ == nullptr) {
        return false;
    }
    Display* display = as_display(display_);
    install_x_error_guard();

    auto focused_is_emulator = [&]() -> bool {
        Window focus = 0;
        int revert = RevertToNone;
        XGetInputFocus(display, &focus, &revert);
        if (focus == 0 || focus == None || focus == PointerRoot) {
            return false;
        }
        if (target_pid_ > 0) {
            if (const auto pid = window_pid(display, focus); pid && *pid > 0) {
                if (pid_in_process_tree(*pid, target_pid_)) {
                    return window_is_viewable(display, focus);
                }
            }
        }
        const auto title = window_title(display, focus);
        auto lower = title;
        for (char& character : lower) {
            character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
        }
        if (lower.find("steamcompmgr") != std::string::npos) {
            return false;
        }
        return title_looks_like_emulator(title) && window_is_viewable(display, focus);
    };

    if (focused_is_emulator()) {
        return true;
    }

    const Window target = find_emulator_window(display, target_pid_);
    if (target == None) {
        if (!logged_focus_miss_) {
            logged_focus_miss_ = true;
            std::cerr
                << "Virtual keyboard: no Ryujinx/yuzu window on " << capture_display_
                << " (pid=" << target_pid_ << ") — Space/FF will not reach the emulator\n";
        }
        return false;
    }

    activate_x_window(display, target);
    if (settle) {
        // Soft-kbd uses ~250ms; gamescope needs a bit longer for _NET_ACTIVE_WINDOW.
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
    }
    if (focused_is_emulator()) {
        return true;
    }
    // One more raise — nested Xwayland under gamescope often needs a second kick.
    activate_x_window(display, target);
    if (settle) {
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
    }
    if (!focused_is_emulator()) {
        if (!logged_focus_miss_) {
            logged_focus_miss_ = true;
            std::cerr
                << "Virtual keyboard: failed to focus Ryujinx/yuzu on " << capture_display_
                << " (pid=" << target_pid_ << ") — F5/Space may miss the emulator\n";
        }
        return false;
    }
    return true;
}

void VirtualKeyboard::set_paused(bool want_paused) {
    if (!plugged_) {
        return;
    }
    // Client sends absolute On/Off (menu open ⇒ pause; overlay edit relaxes). F5 is a
    // toggle, so we only tap when the desired state differs from our last applied value.
    if (want_paused == paused_) {
        // Still reconcile RetroArch from GET_STATUS when possible.
        if (!switch_style_hotkeys_) {
            const auto current = query_retroarch_paused(netcmd_port_);
            if (current.has_value() && *current == want_paused) {
                return;
            }
            if (current.has_value()) {
                // Local cache drifted — fall through and fix.
            } else {
                return;
            }
        } else {
            return;
        }
    }
    if (switch_style_hotkeys_) {
        // Under gamescope, activate can report success while focus never sticks. If we
        // still flip paused_ after a missed F5, the next absolute On/Off inverts the game.
        if (!focus_emulator_window(/*settle=*/true)) {
            std::cerr
                << "EmulatorControl: pause=" << (want_paused ? "on" : "off")
                << " skipped — no emulator focus on " << capture_display_ << '\n';
            return;
        }
        ensure_xtest_display();
        if (display_ == nullptr) {
            return;
        }
        Display* display = as_display(display_);
        const KeyCode code = XKeysymToKeycode(display, XK_F5);
        if (code == 0) {
            return;
        }
        XTestFakeKeyEvent(display, code, True, CurrentTime);
        XFlush(display);
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
        XTestFakeKeyEvent(display, code, False, CurrentTime);
        XFlush(display);
        paused_ = want_paused;
        std::cout
            << "EmulatorControl: pause=" << (want_paused ? "on" : "off")
            << " (XTest F5) on " << capture_display_ << '\n';
        return;
    }
    if (set_retroarch_paused(want_paused, netcmd_port_)) {
        paused_ = want_paused;
        std::cout
            << "EmulatorControl: pause=" << (want_paused ? "on" : "off")
            << " (netcmd " << netcmd_port_ << ")\n";
    } else if (want_paused) {
        if (send_retroarch_netcmd("PAUSE_TOGGLE", netcmd_port_)) {
            paused_ = true;
            std::cout << "EmulatorControl: pause=on via PAUSE_TOGGLE (status unknown)\n";
        }
    }
}

void VirtualKeyboard::set_fast_forward(bool want_on) {
    if (!plugged_) {
        return;
    }
    if (switch_style_hotkeys_) {
        if (want_on == fast_forward_) {
            return;
        }
        // Ryujinx VSync modes cycle Switch → Unbounded → Custom → Switch (F1).
        // Custom refresh is preconfigured at 200% (~2x). From Switch, two taps land
        // on Custom; one tap from Custom returns to Switch. Brief Unbounded blip is OK.
        if (want_on) {
            if (ryujinx_switch_vsync_) {
                xtest_tap_keysym(XK_F1);
                std::this_thread::sleep_for(std::chrono::milliseconds(40));
                xtest_tap_keysym(XK_F1);
                ryujinx_switch_vsync_ = false;
            }
        } else if (!ryujinx_switch_vsync_) {
            xtest_tap_keysym(XK_F1);
            ryujinx_switch_vsync_ = true;
        }
        fast_forward_ = want_on;
        std::cout
            << "EmulatorControl: fast_forward=" << (want_on ? "on" : "off")
            << " (Ryujinx VSync " << (want_on ? "Custom@200%" : "Switch")
            << ") on " << capture_display_ << '\n';
        return;
    }

    // RetroArch session cfg: input_hold_fast_forward=space, toggle=nul.
    // Hold Space for as long as the client wants FF — toggle netcmd desyncs easily.
    // force refresh when already on so menu-close can re-assert after unpause.
    const bool already = (want_on == fast_forward_ && want_on == ff_space_held_);
    if (already && want_on) {
        set_retroarch_ff_space_held(true);
        return;
    }
    if (already) {
        return;
    }
    set_retroarch_ff_space_held(want_on);
    fast_forward_ = want_on;
    std::cout
        << "EmulatorControl: fast_forward=" << (want_on ? "on" : "off")
        << " (hold Space on " << capture_display_ << ")\n";
}

void VirtualKeyboard::apply_emulator_control(const EmulatorControl& control) {
    if (control.pause == EmulatorControlState::On) {
        set_paused(true);
    } else if (control.pause == EmulatorControlState::Off) {
        set_paused(false);
    }
    if (control.fast_forward == EmulatorControlState::On) {
        set_fast_forward(true);
    } else if (control.fast_forward == EmulatorControlState::Off) {
        set_fast_forward(false);
    } else if (
        control.pause == EmulatorControlState::Off &&
        fast_forward_ &&
        !switch_style_hotkeys_) {
        // Unpause can steal focus; re-assert Space hold so RetroArch FF stays active.
        // Ryujinx uses F6 toggles — do not re-tap or we would flip turbo off.
        set_retroarch_ff_space_held(true);
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
                send_retroarch_netcmd(binding.netcmd, netcmd_port_);
            }
            break;
        case RemotedKeyAction::NetcmdPress:
            if (!is_down) {
                break;
            }
            if (binding.key == KeyP) {
                // Desktop remoted P: toggle via explicit set (query when possible).
                if (!switch_style_hotkeys_) {
                    const auto current = query_retroarch_paused(netcmd_port_);
                    set_paused(!(current.value_or(paused_)));
                } else {
                    set_paused(!paused_);
                }
                break;
            }
            if (binding.netcmd != nullptr &&
                send_retroarch_netcmd(binding.netcmd, netcmd_port_)) {
                std::cout
                    << "Keyboard netcmd: "
                    << (binding.key == KeyF1 ? "F1" : "?")
                    << " → " << binding.netcmd << '\n';
            }
            break;
        case RemotedKeyAction::XTestHold:
            if (binding.key == KeySpace && is_down && !logged_ff_) {
                logged_ff_ = true;
                std::cout
                    << "Fast-forward: Space via XTest on " << capture_display_
                    << " (desktop hold / Switch edge taps)\n";
            }
            break;
        case RemotedKeyAction::Ignored:
            break;
        }
    }

    if (previous != next) {
        apply_xtest_edges(previous, next);
    }

    // Space → fast-forward:
    // - Switch/Ryujinx + RetroArch: hold Space while the remoted key is down
    //   (Ryujinx turbo_mode_while_held; RetroArch input_hold_fast_forward).
    //   EmulatorControl owns the hold when fast_forward_ is set; remoted Space
    //   only applies when FF control is off.
    const bool space_down = bit_down(next, KeySpace);
    const bool space_was = bit_down(previous, KeySpace);
    if (space_down != space_was && display_ != nullptr) {
        if (!fast_forward_) {
            xtest_set_keysym(XK_space, space_down);
        }
    }

    last_keys_ = next;
    has_last_ = true;
}

void VirtualKeyboard::release_all() {
    if (ff_space_held_) {
        set_retroarch_ff_space_held(false);
        fast_forward_ = false;
    }
    KeyboardState empty{};
    apply(empty);
}

std::vector<std::string> xtest_display_candidates(const std::string& preferred) {
    std::vector<std::string> names;
    auto add = [&](std::string name) {
        if (name.empty()) {
            return;
        }
        for (const auto& existing : names) {
            if (existing == name) {
                return;
            }
        }
        names.push_back(std::move(name));
    };
    add(preferred);
    // Gamescope nested Xwayland commonly lands on low display numbers; Xvfb slots on :99+.
    for (int display_index = 0; display_index <= 10; ++display_index) {
        add(":" + std::to_string(display_index));
    }
    for (int display_index = 99; display_index <= 110; ++display_index) {
        add(":" + std::to_string(display_index));
    }
    return names;
}

namespace {

std::string normalize_display_name(std::string name) {
    if (name.size() >= 2 && name.compare(0, 2, ".:") == 0) {
        name.erase(0, 1);
    }
    const auto dot = name.find('.');
    if (dot != std::string::npos) {
        name.resize(dot);
    }
    return name;
}

std::string host_desktop_display_name() {
    if (const char* display = std::getenv("DISPLAY"); display != nullptr && *display != '\0') {
        return normalize_display_name(display);
    }
    return ":0";
}

bool is_host_desktop_display_name(const std::string& name) {
    const auto host = host_desktop_display_name();
    if (host.empty() || name.empty()) {
        return false;
    }
    return normalize_display_name(name) == host;
}

std::optional<std::string> display_from_process_environ(int pid) {
    if (pid <= 0) {
        return std::nullopt;
    }
    std::ifstream in("/proc/" + std::to_string(pid) + "/environ");
    if (!in) {
        return std::nullopt;
    }
    std::string blob((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::size_t pos = 0;
    while (pos < blob.size()) {
        const auto end = blob.find('\0', pos);
        const auto entry = blob.substr(
            pos, end == std::string::npos ? std::string::npos : end - pos);
        if (entry.rfind("DISPLAY=", 0) == 0) {
            auto value = entry.substr(8);
            if (!value.empty()) {
                return value;
            }
        }
        if (end == std::string::npos) {
            break;
        }
        pos = end + 1;
    }
    return std::nullopt;
}

std::vector<int> child_pids_of(int parent_pid) {
    std::vector<int> children;
    if (parent_pid <= 0) {
        return children;
    }
    std::error_code error;
    const auto task_dir = std::filesystem::path("/proc") / std::to_string(parent_pid) / "task";
    for (const auto& entry : std::filesystem::directory_iterator(task_dir, error)) {
        if (error) {
            break;
        }
        std::ifstream in(entry.path() / "children");
        if (!in) {
            continue;
        }
        int child = 0;
        while (in >> child) {
            children.push_back(child);
        }
    }
    return children;
}

} // namespace

std::vector<std::string> soft_keyboard_display_candidates(
    const std::string& preferred,
    int owner_pid) {
    std::vector<std::string> names;
    auto add = [&](const std::string& name) {
        if (name.empty() || is_host_desktop_display_name(name)) {
            return;
        }
        for (const auto& existing : names) {
            if (existing == name) {
                return;
            }
        }
        names.push_back(name);
    };

    if (owner_pid > 0) {
        // Children first: nested Xwayland / Ryujinx hold the real dialog DISPLAY.
        std::vector<std::string> from_tree;
        auto consider = [&](int pid) {
            if (const auto display = display_from_process_environ(pid); display) {
                from_tree.push_back(*display);
            }
        };
        for (const int child : child_pids_of(owner_pid)) {
            consider(child);
            for (const int grand : child_pids_of(child)) {
                consider(grand);
                for (const int great : child_pids_of(grand)) {
                    consider(great);
                }
            }
        }
        consider(owner_pid);
        for (const auto& display : from_tree) {
            add(display);
        }
        add(preferred);
        return names;
    }

    // No owner: never blanket-scan every local X socket (that is what dual-prompted
    // concurrent phones). Preferred alone, or the old wide list only when unset.
    if (!preferred.empty()) {
        add(preferred);
        return names;
    }
    return xtest_display_candidates(preferred);
}

void schedule_soft_keyboard(
    std::shared_ptr<SoftKeyboardHostBridge> bridge,
    std::string fallback_text,
    std::string prompt,
    std::string preferred_display,
    int owner_pid) {
    (void)fallback_text; // Callers still pass a profile name; we never invent text.
    if (prompt.empty()) {
        prompt = "The game is asking for text. Enter it with the pad.";
    }
    if (!bridge) {
        bridge = std::make_shared<SoftKeyboardHostBridge>();
    }

    // Weak on purpose: the session owns the bridge, so the watcher retires when the
    // session does instead of one more poller piling up per launch in a persistent lobby.
    std::weak_ptr<SoftKeyboardHostBridge> weak_bridge = bridge;

    std::thread([weak_bridge = std::move(weak_bridge),
                 prompt = std::move(prompt),
                 preferred_display = std::move(preferred_display),
                 owner_pid]() {
        // Once a dialog has been served somewhere, stay on that display. Sibling session
        // slots run their own emulator on their own display and we must not answer theirs.
        std::string pinned_display;

        // Poll hard while a prompt is plausible, then relax so a long session is not
        // paying for a tight X poll all evening.
        constexpr auto kFastInterval = std::chrono::milliseconds(150);
        constexpr auto kIdleInterval = std::chrono::milliseconds(500);
        constexpr int kFastAttempts = 400; // ~60s

        enum class ServeOutcome {
            Abort,       // session bridge gone
            Injected,    // typed into the dialog — wait for it to dismiss
            NeedsReprompt, // cancel/empty/timeout and dialog still wants input
            DialogGone,  // cancel/empty/timeout and dialog no longer Ready
        };

        const auto candidate_displays = [&]() {
            return soft_keyboard_display_candidates(preferred_display, owner_pid);
        };

        const auto try_manual_inject = [&](const std::string& text) {
            std::string trimmed = text;
            if (trimmed.size() > 12) {
                trimmed.resize(12);
            }
            std::cout
                << "Ryujinx Software Keyboard: manual pad OSK text \"" << trimmed
                << "\" — looking for a dialog to fill\n";

            std::vector<std::string> displays;
            if (!pinned_display.empty()) {
                displays.push_back(pinned_display);
            }
            for (const auto& name : candidate_displays()) {
                bool seen = false;
                for (const auto& existing : displays) {
                    if (existing == name) {
                        seen = true;
                        break;
                    }
                }
                if (!seen) {
                    displays.push_back(name);
                }
            }

            for (const auto& display_name : displays) {
                for (int attempt = 0; attempt < 8; ++attempt) {
                    if (try_autofill_on_display(
                            display_name, trimmed, /*allow_any_focused=*/true)) {
                        pinned_display = display_name;
                        return true;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(150));
                }
            }
            std::cerr
                << "Ryujinx Software Keyboard: manual inject failed — no usable dialog found\n";
            return false;
        };

        const auto drain_manual_inject = [&]() {
            auto bridge = weak_bridge.lock();
            if (!bridge) {
                return false;
            }
            auto text = bridge->take_manual_inject();
            bridge.reset();
            if (!text.has_value()) {
                return false;
            }
            try_manual_inject(*text);
            return true;
        };

        // Wait until a dialog is mapped *and* holding keyboard focus (wants typed text),
        // not merely present unmapped in the window tree (boot-time false positives).
        // False once the session drops the bridge.
        // Also services manual pad-OSK injects so the escape hatch works while idle.
        const auto wait_for_dialog = [&](std::string& out) {
            for (int attempt = 0;; ++attempt) {
                if (weak_bridge.expired()) {
                    return false;
                }
                if (drain_manual_inject()) {
                    // Inject may have dismissed the dialog; keep waiting for the next one.
                    continue;
                }
                if (!pinned_display.empty()) {
                    const auto result = probe_text_dialog(pinned_display);
                    if (result == TextDialogProbe::Ready) {
                        out = pinned_display;
                        return true;
                    }
                    if (result == TextDialogProbe::Unavailable) {
                        // Emulator restarted onto a different display; rescan.
                        pinned_display.clear();
                    }
                } else {
                    // Rebuild each tick: nested Xwayland may appear after gamescope start.
                    for (const auto& name : candidate_displays()) {
                        const auto result = probe_text_dialog(name);
                        if (result == TextDialogProbe::Ready) {
                            out = name;
                            return true;
                        }
                    }
                }
                std::this_thread::sleep_for(
                    attempt < kFastAttempts ? kFastInterval : kIdleInterval);
            }
        };

        // False once the session drops the bridge.
        const auto serve_dialog = [&](const std::string& display_name) -> ServeOutcome {
            SoftKeyboardRequest request;
            {
                auto bridge = weak_bridge.lock();
                if (!bridge) {
                    return ServeOutcome::Abort;
                }
                {
                    std::lock_guard lock(bridge->mutex);
                    // Blank field: the player is being asked to type a name, and
                    // prefilling it just means erasing it on a pad keyboard first.
                    request = bridge->make_request(prompt, /*initial_text=*/{}, 12);
                }
                bridge->publish_request(request);
            }
            std::cout
                << "Ryujinx Software Keyboard: focused text dialog on " << display_name
                << " — requesting pad OSK (id=" << request.request_id << ")\n";

            std::optional<SoftKeyboardResponse> response;
            std::optional<std::string> manual_text;
            for (int wait = 0; wait < 360; ++wait) {
                auto bridge = weak_bridge.lock();
                if (!bridge) {
                    return ServeOutcome::Abort;
                }
                // Prefer an explicit answer to this host-driven prompt; otherwise accept a
                // concurrent manual pad-OSK submit as the typed value.
                response = bridge->take_response();
                if (response.has_value() && response->request_id == request.request_id) {
                    break;
                }
                response.reset();
                manual_text = bridge->take_manual_inject();
                if (manual_text.has_value()) {
                    break;
                }
                bridge.reset();
                // Never re-publish while waiting: the request goes out over the TCP control
                // stream, and a resend made the client tear down and rebuild the pad OSK
                // every 5s, wiping whatever the player had typed.
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }

            std::string text;
            const bool from_manual = manual_text.has_value() && !manual_text->empty();
            const bool from_accept = response.has_value() && response->accepted &&
                !response->text.empty();
            if (from_manual) {
                text = *manual_text;
                std::cout
                    << "Ryujinx Software Keyboard: using manual pad OSK text for id="
                    << request.request_id << '\n';
            } else if (from_accept) {
                text = response->text;
            } else {
                // Cancel, empty submit, or timeout: never invent a name. The game still
                // owns the dialog — if it is Ready, publish another SoftKeyboardRequest.
                if (!response.has_value() && !manual_text.has_value()) {
                    std::cerr
                        << "Ryujinx Software Keyboard: no pad OSK response for id="
                        << request.request_id << " — not injecting\n";
                } else {
                    std::cout
                        << "Ryujinx Software Keyboard: pad OSK cancelled/empty for id="
                        << request.request_id << " — not injecting\n";
                }
                if (auto bridge = weak_bridge.lock()) {
                    bridge->clear();
                }
                if (probe_text_dialog(display_name) == TextDialogProbe::Ready) {
                    return ServeOutcome::NeedsReprompt;
                }
                return ServeOutcome::DialogGone;
            }

            if (text.size() > 12) {
                text.resize(12);
            }

            bool injected = false;
            for (int attempt = 0; attempt < 20 && !injected; ++attempt) {
                if (try_autofill_on_display(
                        display_name, text, /*allow_any_focused=*/from_manual)) {
                    injected = true;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
            }
            if (!injected) {
                std::cerr << "Ryujinx Software Keyboard: failed to inject text into dialog\n";
                if (auto bridge = weak_bridge.lock()) {
                    bridge->clear();
                }
                // Injection failed but dialog may still be up — re-prompt rather than
                // inventing text or assuming the game moved on.
                if (probe_text_dialog(display_name) == TextDialogProbe::Ready) {
                    return ServeOutcome::NeedsReprompt;
                }
                return ServeOutcome::DialogGone;
            }
            if (auto bridge = weak_bridge.lock()) {
                bridge->clear();
            }
            return ServeOutcome::Injected;
        };

        // Re-arming while the answered dialog is still up would instantly re-prompt for
        // the one we just filled in. Still accept manual injects here — a follow-up
        // prompt (Pokemon nickname confirm / retry) may need the escape hatch before
        // the previous overlay fully disappears from our title probe.
        const auto wait_for_dialog_to_close = [&](const std::string& display_name) {
            for (int attempt = 0; attempt < 400; ++attempt) { // ~60s
                if (weak_bridge.expired()) {
                    return;
                }
                if (drain_manual_inject()) {
                    continue;
                }
                if (probe_text_dialog(display_name) != TextDialogProbe::Ready) {
                    return;
                }
                std::this_thread::sleep_for(kFastInterval);
            }
        };

        // Games ask more than once: declining the "is this right?" confirmation reopens
        // the same prompt, so keep serving dialogs for as long as the session lives.
        // Cancel with empty input while the dialog is still Ready → new SoftKeyboardRequest
        // (game still blocked); we do not invent nicknames.
        try {
            while (true) {
                std::string found_display;
                if (!wait_for_dialog(found_display)) {
                    return;
                }
                pinned_display = found_display;
                for (;;) {
                    const auto outcome = serve_dialog(found_display);
                    if (outcome == ServeOutcome::Abort) {
                        return;
                    }
                    if (outcome == ServeOutcome::NeedsReprompt) {
                        // Brief settle so the client can dismiss the cancelled OSK
                        // before the next SoftKeyboardRequest arrives.
                        std::this_thread::sleep_for(std::chrono::milliseconds(250));
                        if (weak_bridge.expired()) {
                            return;
                        }
                        if (probe_text_dialog(found_display) != TextDialogProbe::Ready) {
                            break;
                        }
                        continue;
                    }
                    if (outcome == ServeOutcome::DialogGone) {
                        break;
                    }
                    // Injected — wait until this dialog dismisses before arming again.
                    wait_for_dialog_to_close(found_display);
                    break;
                }
            }
        } catch (...) {
        }
    }).detach();
}

void ensure_soft_keyboard(
    std::shared_ptr<SoftKeyboardHostBridge>& bridge,
    std::string fallback_text,
    std::string prompt,
    std::string preferred_display,
    int owner_pid) {
    if (!bridge) {
        bridge = std::make_shared<SoftKeyboardHostBridge>();
    }
    schedule_soft_keyboard(
        bridge,
        std::move(fallback_text),
        std::move(prompt),
        std::move(preferred_display),
        owner_pid);
}

} // namespace archstreamer

#endif // !_WIN32 — Windows backends: windows_virtual_keyboard.cpp / windows_soft_keyboard.cpp
