#ifndef _WIN32
#include "host/virtual_keyboard.hpp"
#include "host/soft_keyboard_host.hpp"

#include "host/retroarch_netcmd.hpp"

#include <chrono>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
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
        << " (Space→XTest hold-FF, F8→Yuzu continuous FF, P→pause, F1→menu; arrows/Enter/Esc→XTest)\n";
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
    XSetIOErrorExitHandler(
        display,
        [](Display*, void*) {},
        nullptr);

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
    XSetIOErrorExitHandler(
        display,
        [](Display*, void*) {},
        nullptr);

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

std::string title_case_fallback(std::string text) {
    if (text.size() > 12) {
        text.resize(12);
    }
    if (text.empty()) {
        return "Player";
    }
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
    return text;
}

std::vector<std::string> soft_keyboard_display_candidates(const std::string& preferred) {
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

} // namespace

void schedule_ryujinx_soft_keyboard(
    std::shared_ptr<SoftKeyboardHostBridge> bridge,
    std::string fallback_text,
    std::string prompt,
    std::string preferred_display) {
    fallback_text = title_case_fallback(std::move(fallback_text));
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
                 fallback_text = std::move(fallback_text),
                 prompt = std::move(prompt),
                 preferred_display = std::move(preferred_display)]() {
        struct DisplayProbe {
            std::string name;
            // Attempt index before which we do not retry XOpenDisplay on this slot.
            int retry_at = 0;
        };
        std::vector<DisplayProbe> probes;
        for (auto& name : soft_keyboard_display_candidates(preferred_display)) {
            probes.push_back({std::move(name), 0});
        }
        // Once a dialog has been served somewhere, stay on that display. Sibling session
        // slots run their own emulator on their own display and we must not answer theirs.
        std::string pinned_display;

        // Poll hard while a prompt is plausible, then relax so a long session is not
        // paying for a tight X poll all evening.
        constexpr auto kFastInterval = std::chrono::milliseconds(150);
        constexpr auto kIdleInterval = std::chrono::milliseconds(500);
        constexpr int kFastAttempts = 400; // ~60s
        constexpr int kUnavailableBackoff = 13; // ~2s before re-probing a dead slot

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
            for (const auto& probe : probes) {
                bool seen = false;
                for (const auto& existing : displays) {
                    if (existing == probe.name) {
                        seen = true;
                        break;
                    }
                }
                if (!seen) {
                    displays.push_back(probe.name);
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
            for (auto& probe : probes) {
                probe.retry_at = 0;
            }
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
                    // Most candidate slots are empty, so back those off: a tick then only
                    // pays for the one or two displays that actually exist.
                    for (auto& probe : probes) {
                        if (attempt < probe.retry_at) {
                            continue;
                        }
                        const auto result = probe_text_dialog(probe.name);
                        if (result == TextDialogProbe::Unavailable) {
                            probe.retry_at = attempt + kUnavailableBackoff;
                        } else if (result == TextDialogProbe::Ready) {
                            out = probe.name;
                            return true;
                        }
                    }
                }
                std::this_thread::sleep_for(
                    attempt < kFastAttempts ? kFastInterval : kIdleInterval);
            }
        };

        // False once the session drops the bridge.
        const auto serve_dialog = [&](const std::string& display_name) {
            SoftKeyboardRequest request;
            {
                auto bridge = weak_bridge.lock();
                if (!bridge) {
                    return false;
                }
                {
                    std::lock_guard lock(bridge->mutex);
                    // Blank field: the player is being asked to type a name, and
                    // prefilling it just means erasing it on a pad keyboard first.
                    // fallback_text is only for the no-answer path below.
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
                    return false;
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

            std::string text = fallback_text;
            if (manual_text.has_value() && !manual_text->empty()) {
                text = *manual_text;
                std::cout
                    << "Ryujinx Software Keyboard: using manual pad OSK text for id="
                    << request.request_id << '\n';
            } else if (response.has_value() && response->accepted && !response->text.empty()) {
                text = response->text;
            } else if (!response.has_value() && !manual_text.has_value()) {
                std::cerr
                    << "Ryujinx Software Keyboard: no pad OSK response; using fallback \""
                    << fallback_text << "\"\n";
            } else {
                std::cerr
                    << "Ryujinx Software Keyboard: pad OSK cancelled; using fallback \""
                    << fallback_text << "\"\n";
            }
            if (text.size() > 12) {
                text.resize(12);
            }

            bool injected = false;
            for (int attempt = 0; attempt < 20 && !injected; ++attempt) {
                if (try_autofill_on_display(
                        display_name, text, /*allow_any_focused=*/manual_text.has_value())) {
                    injected = true;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
            }
            if (!injected) {
                std::cerr << "Ryujinx Software Keyboard: failed to inject text into dialog\n";
            }
            if (auto bridge = weak_bridge.lock()) {
                bridge->clear();
            }
            return true;
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
        try {
            while (true) {
                std::string found_display;
                if (!wait_for_dialog(found_display)) {
                    return;
                }
                pinned_display = found_display;
                if (!serve_dialog(found_display)) {
                    return;
                }
                wait_for_dialog_to_close(found_display);
            }
        } catch (...) {
        }
    }).detach();
}

void ensure_ryujinx_soft_keyboard(
    std::shared_ptr<SoftKeyboardHostBridge>& bridge,
    std::string fallback_text,
    std::string prompt,
    std::string preferred_display) {
    if (!bridge) {
        bridge = std::make_shared<SoftKeyboardHostBridge>();
    }
    schedule_ryujinx_soft_keyboard(
        bridge,
        std::move(fallback_text),
        std::move(prompt),
        std::move(preferred_display));
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

void ensure_ryujinx_soft_keyboard(
    std::shared_ptr<SoftKeyboardHostBridge>& bridge,
    std::string fallback_text,
    std::string prompt,
    std::string preferred_display) {
    (void)bridge;
    (void)fallback_text;
    (void)prompt;
    (void)preferred_display;
}

} // namespace archstreamer

#endif
