#ifndef _WIN32
#include "host/virtual_keyboard.hpp"
#include "host/soft_keyboard_host.hpp"
#include "host/nds/melonds_ctrl_client.hpp"

#include "host/retroarch_netcmd.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <unistd.h>

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/extensions/XTest.h>

#include <cstdio>

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
    ff_key_held_ = false;
    ryujinx_vsync_mode_ = 0;
    ryujinx_switch_vsync_ = true;
    std::cout
        << "Virtual keyboard ready on " << capture_display_
        << (switch_style_hotkeys_
                ? " (Ryujinx FF→F1 VSync Custom@200%, P→F5 pause; arrows/Enter/Esc→XTest)\n"
                : melonds_style_hotkeys_
                    ? " (melonDS FF→hold Space, Pause→F5, Swap→F6; arrows/Enter/Esc→XTest)\n"
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
    // Unused — Space/F6 hold is handled in apply() / set_ff_key_held().
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

unsigned long VirtualKeyboard::ff_hold_keysym() const {
    // RetroArch session cfg: input_hold_fast_forward=space.
    // Ryujinx EmulatorControl FF uses F1 VSync taps (see set_fast_forward).
    return XK_space;
}

void VirtualKeyboard::set_ff_key_held(bool want_held) {
    const unsigned long keysym = ff_hold_keysym();
    if (want_held == ff_key_held_) {
        if (want_held) {
            // Re-assert focus + down in case the capture window ate the key.
            xtest_set_keysym(keysym, true);
        }
        return;
    }
    xtest_set_keysym(keysym, want_held);
    ff_key_held_ = want_held;
}

void VirtualKeyboard::reassert_fast_forward_hold() {
    if (!plugged_ || !fast_forward_) {
        return;
    }
    if (switch_style_hotkeys_) {
        // Ryujinx FF is tap-based (F1), not a hold key.
        return;
    }
    // RetroArch Space hold and melonDS HK_FastForward=Space.
    set_ff_key_held(true);
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
        lower.find("melonds") != std::string::npos ||
        lower.find("melon ds") != std::string::npos ||
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

// Ryujinx ErrorAppletWindow: Title is "Error Number: N", "Error Code: …", or
// "Details: …". Separate from soft-keyboard ContentDialogOverlayWindow.
bool is_error_applet_dialog_title(const std::string& title) {
    return title.rfind("Error Number:", 0) == 0 ||
        title.rfind("Error Code:", 0) == 0 ||
        title.rfind("Details:", 0) == 0;
}

std::optional<Window> find_viewable_titled(
    Display* display,
    bool (*match)(const std::string&)) {
    std::vector<std::pair<Window, std::string>> windows;
    collect_windows(display, DefaultRootWindow(display), windows);
    for (const auto& [window, title] : windows) {
        if (match(title) && window_is_viewable(display, window)) {
            return window;
        }
    }
    return std::nullopt;
}

void xtest_tap_keysym_display(Display* display, KeySym keysym) {
    const KeyCode code = XKeysymToKeycode(display, keysym);
    if (code == 0) {
        return;
    }
    XTestFakeKeyEvent(display, code, True, CurrentTime);
    XFlush(display);
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    XTestFakeKeyEvent(display, code, False, CurrentTime);
    XFlush(display);
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
}

// Error applet buttons are [Details?, OK] with OK last. Focus the dialog, then
 // Tab→Return (or End→Return) so we hit OK rather than Details.
bool try_dismiss_error_applet_on_display(const std::string& display_name) {
    install_x_error_guard();
    Display* display = XOpenDisplay(display_name.c_str());
    if (display == nullptr) {
        return false;
    }
    install_x_io_exit_guard(display);

    const auto target = find_viewable_titled(display, is_error_applet_dialog_title);
    if (!target.has_value()) {
        XCloseDisplay(display);
        return false;
    }
    const auto title = window_title(display, *target);
    activate_x_window(display, *target);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Prefer End (last button = OK) then Return; fall back to Tab+Return.
    xtest_tap_keysym_display(display, XK_End);
    xtest_tap_keysym_display(display, XK_Return);
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    if (!find_viewable_titled(display, is_error_applet_dialog_title).has_value()) {
        XCloseDisplay(display);
        std::cout
            << "Ryujinx Error Applet: dismissed \"" << title
            << "\" on " << display_name << " (End+Return)\n";
        return true;
    }
    xtest_tap_keysym_display(display, XK_Tab);
    xtest_tap_keysym_display(display, XK_Return);
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    const bool gone =
        !find_viewable_titled(display, is_error_applet_dialog_title).has_value();
    XCloseDisplay(display);
    if (gone) {
        std::cout
            << "Ryujinx Error Applet: dismissed \"" << title
            << "\" on " << display_name << " (Tab+Return)\n";
    }
    return gone;
}

std::string trim_ascii(std::string text) {
    while (!text.empty() &&
           std::isspace(static_cast<unsigned char>(text.front()))) {
        text.erase(text.begin());
    }
    while (!text.empty() &&
           std::isspace(static_cast<unsigned char>(text.back()))) {
        text.pop_back();
    }
    return text;
}

std::string lower_ascii(std::string text) {
    for (char& character : text) {
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }
    return text;
}

// Pokemon HeaderText uses (♀)'s / (♂)'s. Tesseract mangles only the symbol
// glyph (e.g. ♂ → "0'fs", ♀ → "o" / "dfs"); parentheses are already in the
 // string. Replace the junk token alone — do not wrap or re-parenthesize.
std::string fix_swkbd_gender_symbols(std::string text) {
    constexpr const char* kFemale = "\xE2\x99\x80"; // ♀
    constexpr const char* kMale = "\xE2\x99\x82";   // ♂
    if (text.find(kFemale) != std::string::npos || text.find(kMale) != std::string::npos) {
        return text;
    }

    // Closed "(JUNK)'s" → "(♀/♂)'s"
    static const std::regex kMaleClosed(
        R"(\(\s*(?:0'fs|0fs|o'fs|ofs|0'f|o\^|\^|o\+|o\|)\s*\)\s*'?s\b)");
    static const std::regex kFemaleClosed(
        R"(\(\s*(?:[oOq]|o'|dfs?|d'?s|9ys?)\s*\)\s*'?s\b)");

    // OCR often eats ")'s" into the junk: "(0'fs nickname" → keep "("; fix token.
    static const std::regex kMaleOpen(
        R"(\(\s*(?:0'fs|0fs|o'fs|ofs|0'f|o\^|\^|o\+|o\|)\s+)");
    static const std::regex kFemaleOpen(
        R"(\(\s*(?:dfs?|d'?s|9ys?)\s+)");

    const std::string male_closed = std::string("(") + kMale + ")'s";
    const std::string female_closed = std::string("(") + kFemale + ")'s";
    // Open forms restore the missing ")'s" that OCR merged into the glyph junk.
    const std::string male_open = std::string("(") + kMale + ")'s ";
    const std::string female_open = std::string("(") + kFemale + ")'s ";

    text = std::regex_replace(text, kMaleClosed, male_closed);
    text = std::regex_replace(text, kFemaleClosed, female_closed);
    text = std::regex_replace(text, kMaleOpen, male_open);
    text = std::regex_replace(text, kFemaleOpen, female_open);
    text = std::regex_replace(text, std::regex(R"(\s+)"), " ");
    return trim_ascii(text);
}

// Pick HeaderText from tesseract lines on the Avalonia swkbd ContentDialog.
// Layout: title "Software Keyboard", HeaderText, validation ("Must be…"), OK/Cancel.
std::string prompt_from_swkbd_ocr(const std::string& ocr_text) {
    std::istringstream stream(ocr_text);
    std::string line;
    std::string best;
    while (std::getline(stream, line)) {
        line = trim_ascii(line);
        if (line.size() < 3) {
            continue;
        }
        const auto lower = lower_ascii(line);
        if (lower.find("software keyboard") != std::string::npos) {
            continue;
        }
        if (lower.rfind("must be", 0) == 0) {
            continue;
        }
        if (lower == "ok" || lower == "cancel" || lower == "submit") {
            continue;
        }
        if (line.find('?') != std::string::npos) {
            return fix_swkbd_gender_symbols(line);
        }
        if (best.empty() || line.size() > best.size()) {
            best = line;
        }
    }
    return fix_swkbd_gender_symbols(best);
}

bool write_pgm_u8(
    const std::filesystem::path& path,
    int width,
    int height,
    const std::vector<std::uint8_t>& pixels) {
    if (width <= 0 || height <= 0 ||
        pixels.size() != static_cast<std::size_t>(width) * static_cast<std::size_t>(height)) {
        return false;
    }
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        return false;
    }
    out << "P5\n" << width << ' ' << height << "\n255\n";
    out.write(reinterpret_cast<const char*>(pixels.data()),
              static_cast<std::streamsize>(pixels.size()));
    return static_cast<bool>(out);
}

// Capture the focused ContentDialog overlay and OCR HeaderText.
// gamescope/Xwayland often yields a blank frame from XGetImage right as the
// dialog maps; ImageMagick `import -window` matches what we can read by hand.
// Retries until ink appears. Needs `import`/`convert` (imagemagick) + `tesseract`.
std::string ocr_soft_keyboard_prompt_on_display(
    const std::string& display_name,
    Window dialog) {
    const auto dir = std::filesystem::temp_directory_path() / "archstreamer-swkbd";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const auto png = dir / "dialog.png";
    const auto pgm = dir / "dialog.pgm";

    const char* home = std::getenv("HOME");
    std::string path_prefix = "PATH=\"";
    if (home != nullptr && home[0] != '\0') {
        path_prefix += home;
        path_prefix += "/.local/bin:";
    }
    path_prefix += "${PATH}\" ";

    auto run_shell = [](const std::string& command) -> int {
        return std::system(command.c_str());
    };

    auto tesseract_prompt = [&]() -> std::string {
        std::ostringstream command;
        command << path_prefix << "tesseract " << pgm.string()
                << " stdout -l eng --psm 6 2>/dev/null";
        FILE* pipe = popen(command.str().c_str(), "r");
        if (pipe == nullptr) {
            return {};
        }
        std::string ocr;
        char buffer[512];
        while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
            ocr += buffer;
        }
        const int status = pclose(pipe);
        if (status != 0 || ocr.empty()) {
            return {};
        }
        return prompt_from_swkbd_ocr(ocr);
    };

    // Give Avalonia a beat to paint HeaderText before the first grab.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    for (int attempt = 0; attempt < 8; ++attempt) {
        if (attempt > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
        }

        // 1) Preferred: ImageMagick import of the overlay window (works under gamescope).
        bool got_image = false;
        {
            std::ostringstream command;
            command << "DISPLAY=" << display_name << " import -window "
                    << "0x" << std::hex << static_cast<unsigned long>(dialog) << std::dec
                    << " " << png.string() << " 2>/dev/null";
            if (run_shell(command.str()) == 0 &&
                std::filesystem::exists(png) &&
                std::filesystem::file_size(png, ec) > 1024) {
                std::ostringstream convert;
                convert << "convert " << png.string()
                        << " -gravity Center -crop 55%x50%+0-20 +repage "
                        << "-colorspace Gray -negate -threshold 70% "
                        << pgm.string() << " 2>/dev/null";
                if (run_shell(convert.str()) == 0 && std::filesystem::exists(pgm, ec)) {
                    got_image = true;
                }
            }
        }

        // 2) Fallback: XGetImage (may be blank on the first frames).
        if (!got_image) {
            install_x_error_guard();
            Display* display = XOpenDisplay(display_name.c_str());
            if (display == nullptr) {
                continue;
            }
            install_x_io_exit_guard(display);

            XWindowAttributes attrs{};
            if (!XGetWindowAttributes(display, dialog, &attrs) ||
                attrs.width < 64 || attrs.height < 64) {
                XCloseDisplay(display);
                continue;
            }

            XImage* image = XGetImage(
                display, dialog, 0, 0, attrs.width, attrs.height, AllPlanes, ZPixmap);
            if (image == nullptr) {
                XCloseDisplay(display);
                continue;
            }

            const int full_w = attrs.width;
            const int full_h = attrs.height;
            const int crop_w = std::max(64, full_w * 55 / 100);
            const int crop_h = std::max(64, full_h * 50 / 100);
            const int origin_x = (full_w - crop_w) / 2;
            const int origin_y = std::max(0, (full_h - crop_h) / 2 - full_h / 40);

            std::vector<std::uint8_t> pixels(
                static_cast<std::size_t>(crop_w) * static_cast<std::size_t>(crop_h));
            std::size_t ink = 0;
            for (int y = 0; y < crop_h; ++y) {
                for (int x = 0; x < crop_w; ++x) {
                    const unsigned long pixel =
                        XGetPixel(image, origin_x + x, origin_y + y);
                    const unsigned r = (pixel & image->red_mask) /
                        (image->red_mask ? (image->red_mask / 255) : 1);
                    const unsigned g = (pixel & image->green_mask) /
                        (image->green_mask ? (image->green_mask / 255) : 1);
                    const unsigned b = (pixel & image->blue_mask) /
                        (image->blue_mask ? (image->blue_mask / 255) : 1);
                    unsigned luma = (r * 30 + g * 59 + b * 11) / 100;
                    if (luma > 255) {
                        luma = 255;
                    }
                    luma = 255 - luma;
                    const auto value =
                        static_cast<std::uint8_t>(luma < 180 ? 0 : 255);
                    pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(crop_w) +
                           static_cast<std::size_t>(x)] = value;
                    if (value == 0) {
                        ++ink;
                    }
                }
            }
            XDestroyImage(image);
            XCloseDisplay(display);

            // Flat dark frame → all-white after invert; wait and retry.
            if (ink * 200 < pixels.size()) { // <0.5% ink
                continue;
            }
            if (!write_pgm_u8(pgm, crop_w, crop_h, pixels)) {
                continue;
            }
            got_image = true;
        }

        if (!got_image) {
            continue;
        }

        if (auto prompt = tesseract_prompt(); !prompt.empty()) {
            return prompt;
        }
    }

    std::cerr
        << "Ryujinx Software Keyboard: OCR failed to read dialog prompt on "
        << display_name << " (need imagemagick + tesseract)\n";
    return {};
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
                << "Virtual keyboard: no emulator window on " << capture_display_
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
                << "Virtual keyboard: failed to focus emulator on " << capture_display_
                << " (pid=" << target_pid_ << ") — F5/Space may miss the emulator\n";
        }
        return false;
    }
    return true;
}

void VirtualKeyboard::set_paused(bool want_paused, bool force) {
    if (!plugged_) {
        return;
    }
    // melonDS: absolute PAUSE via --archstreamer-ctrl (emuPause/emuUnpause). F5 is a
    // toggle and cannot implement drawer On/Off reliably; it also fights PauseToggle.
    if (melonds_style_hotkeys_ && !melonds_ctrl_name_.empty()) {
        MelonDsCtrlClient client(melonds_ctrl_name_);
        if (const auto current = client.query_paused(); current.has_value()) {
            if (*current == want_paused && !force) {
                paused_ = want_paused;
                return;
            }
        } else if (want_paused == paused_ && !force) {
            return;
        }
        if (client.set_paused(want_paused)) {
            paused_ = want_paused;
            std::cout
                << "EmulatorControl: pause=" << (want_paused ? "on" : "off")
                << " (melonDS ctrl PAUSE"
                << (force ? ", force" : "") << ") on " << capture_display_ << '\n';
            return;
        }
        std::cerr
            << "EmulatorControl: melonDS ctrl PAUSE failed (" << client.last_error()
            << ") — falling back to XTest F5\n";
    }
    // Client sends absolute On/Off. F5 is a toggle: tap only when desired state
    // differs from our cache. force must NOT re-tap on a match — that inverts an
    // already-correct (or already-assumed) game. Missed taps are a focus/delivery
    // problem; blind force retries make P-spam feel random.
    if (want_paused == paused_) {
        if (switch_style_hotkeys_ || melonds_style_hotkeys_) {
            return;
        }
        if (!force) {
            const auto current = query_retroarch_paused(netcmd_port_);
            if (current.has_value() && *current == want_paused) {
                return;
            }
            if (!current.has_value()) {
                return;
            }
            // Local cache drifted — fall through and fix via netcmd.
        } else {
            // RetroArch: force may push absolute pause via netcmd below.
        }
    }
    if (switch_style_hotkeys_ || melonds_style_hotkeys_) {
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
            << " (XTest F5" << (melonds_style_hotkeys_ ? "/melonDS" : "")
            << (force ? ", force" : "") << ") on " << capture_display_ << '\n';
        return;
    }
    if (set_retroarch_paused(want_paused, netcmd_port_)) {
        paused_ = want_paused;
        std::cout
            << "EmulatorControl: pause=" << (want_paused ? "on" : "off")
            << " (netcmd " << netcmd_port_ << (force ? ", force" : "") << ")\n";
    } else if (want_paused) {
        if (send_retroarch_netcmd("PAUSE_TOGGLE", netcmd_port_)) {
            paused_ = true;
            std::cout << "EmulatorControl: pause=on via PAUSE_TOGGLE (status unknown)\n";
        }
    }
}

void VirtualKeyboard::set_fast_forward(bool want_on, bool force) {
    if (!plugged_) {
        return;
    }
    if (switch_style_hotkeys_) {
        // Ryujinx: F1 cycles Switch(0) → Unbounded(1) → Custom@200%(2) → Switch.
        // On = land on Custom (2 taps from Switch). Off = return to Switch
        // (1 tap from Custom, or 2 from Unbounded if an On tap was missed).
        // Never re-cycle when the cache already matches — force retries desync F1
        // and leave the game in Custom@200% while the client thinks FF is off.
        if (want_on == fast_forward_) {
            return;
        }
        if (!focus_emulator_window(/*settle=*/true)) {
            std::cerr
                << "EmulatorControl: fast_forward=" << (want_on ? "on" : "off")
                << " skipped — no emulator focus on " << capture_display_ << '\n';
            // Do not update caches — a later real edge can still apply.
            return;
        }
        if (want_on) {
            if (ryujinx_vsync_mode_ == 0) {
                xtest_tap_keysym(XK_F1);
                std::this_thread::sleep_for(std::chrono::milliseconds(40));
                xtest_tap_keysym(XK_F1);
                ryujinx_vsync_mode_ = 2; // Custom
            }
            ryujinx_switch_vsync_ = false;
            fast_forward_ = true;
        } else {
            // Return to Switch. From Custom need 1 tap; from Unbounded need 2
            // (On tap missed → landed Unbounded while we thought Custom).
            int guard = 0;
            while (ryujinx_vsync_mode_ != 0 && guard < 2) {
                xtest_tap_keysym(XK_F1);
                std::this_thread::sleep_for(std::chrono::milliseconds(40));
                ryujinx_vsync_mode_ = static_cast<std::uint8_t>((ryujinx_vsync_mode_ + 1) % 3);
                ++guard;
            }
            ryujinx_switch_vsync_ = (ryujinx_vsync_mode_ == 0);
            fast_forward_ = false;
        }
        std::cout
            << "EmulatorControl: fast_forward=" << (want_on ? "on" : "off")
            << " (Ryujinx VSync mode=" << static_cast<int>(ryujinx_vsync_mode_)
            << (want_on ? " Custom@200%" : " Switch")
            << (force ? ", force" : "") << ") on " << capture_display_ << '\n';
        return;
    }

    // RetroArch hold-FF and melonDS HK_FastForward — Space while held.
    const bool already = (want_on == fast_forward_ && want_on == ff_key_held_);
    if (already && !force) {
        return;
    }
    if (melonds_style_hotkeys_) {
        // Same focus gate as pause: under gamescope a missed Space strands FF state.
        if (!focus_emulator_window(/*settle=*/true)) {
            std::cerr
                << "EmulatorControl: fast_forward=" << (want_on ? "on" : "off")
                << " skipped — no emulator focus on " << capture_display_ << '\n';
            return;
        }
    }
    if (already && force && want_on) {
        set_ff_key_held(true);
        return;
    }
    set_ff_key_held(want_on);
    fast_forward_ = want_on;
    std::cout
        << "EmulatorControl: fast_forward=" << (want_on ? "on" : "off")
        << " (hold Space" << (melonds_style_hotkeys_ ? "/melonDS" : "")
        << (force ? ", force" : "") << ") on " << capture_display_ << '\n';
}

void VirtualKeyboard::trigger_screen_swap() {
    if (!plugged_) {
        return;
    }
    if (!melonds_style_hotkeys_) {
        std::cerr << "EmulatorControl: screen_swap ignored (not melonDS)\n";
        return;
    }
    if (!focus_emulator_window(/*settle=*/true)) {
        std::cerr
            << "EmulatorControl: screen_swap skipped — no emulator focus on "
            << capture_display_ << '\n';
        return;
    }
    xtest_tap_keysym(XK_F6);
    std::cout
        << "EmulatorControl: screen_swap (XTest F6/melonDS) on " << capture_display_
        << '\n';
}

void VirtualKeyboard::trigger_pause_toggle() {
    if (!plugged_) {
        return;
    }
    if (melonds_style_hotkeys_ && !melonds_ctrl_name_.empty()) {
        MelonDsCtrlClient client(melonds_ctrl_name_);
        if (client.toggle_paused()) {
            if (const auto current = client.query_paused(); current.has_value()) {
                paused_ = *current;
            } else {
                paused_ = !paused_;
            }
            std::cout
                << "EmulatorControl: pause_toggle → " << (paused_ ? "on" : "off")
                << " (melonDS ctrl PAUSE) on " << capture_display_ << '\n';
            return;
        }
        std::cerr
            << "EmulatorControl: melonDS ctrl PAUSE toggle failed (" << client.last_error()
            << ") — falling back to XTest F5\n";
    }
    if (switch_style_hotkeys_ || melonds_style_hotkeys_) {
        // One P → one F5. Absolute On/Off + host cache desyncs whenever a tap misses;
        // FF works because it is a level hold (Space), not a toggle.
        if (!focus_emulator_window(/*settle=*/true)) {
            std::cerr
                << "EmulatorControl: pause_toggle skipped — no emulator focus on "
                << capture_display_ << '\n';
            return;
        }
        xtest_tap_keysym(XK_F5);
        paused_ = !paused_;
        std::cout
            << "EmulatorControl: pause_toggle → " << (paused_ ? "on" : "off")
            << " (XTest F5" << (melonds_style_hotkeys_ ? "/melonDS" : "")
            << ") on " << capture_display_ << '\n';
        return;
    }
    if (send_retroarch_netcmd("PAUSE_TOGGLE", netcmd_port_)) {
        paused_ = !paused_;
        std::cout
            << "EmulatorControl: pause_toggle → " << (paused_ ? "on" : "off")
            << " (netcmd " << netcmd_port_ << ")\n";
    }
}

void VirtualKeyboard::apply_emulator_control(const EmulatorControl& control) {
    const bool force = control.force != 0;
    if (control.pause == EmulatorControlState::On) {
        set_paused(true, force);
    } else if (control.pause == EmulatorControlState::Off) {
        set_paused(false, force);
    }
    if (control.fast_forward == EmulatorControlState::On) {
        set_fast_forward(true, force);
    } else if (control.fast_forward == EmulatorControlState::Off) {
        set_fast_forward(false, force);
    }
    // Do not reassert FF on pause-off. FF is only driven by explicit On/Off from
    // the client (hold or menu latch). Spurious re-holds fight that model.
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
                // Pause is EmulatorControl-only on every backend (RA netcmd / F5).
                // Remoted P from a client is a bug — strip it client-side.
                std::cerr
                    << "Keyboard: remoted P ignored — use EmulatorControl pause\n";
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

    // Space → fast-forward hold when EmulatorControl FF is off (RetroArch).
    // Ryujinx FF is EmulatorControl F1 taps only.
    const bool space_down = bit_down(next, KeySpace);
    const bool space_was = bit_down(previous, KeySpace);
    if (space_down != space_was && display_ != nullptr) {
        if (!fast_forward_ && !switch_style_hotkeys_) {
            xtest_set_keysym(XK_space, space_down);
        }
    }

    last_keys_ = next;
    has_last_ = true;
}

void VirtualKeyboard::release_all() {
    if (ff_key_held_) {
        set_ff_key_held(false);
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

std::optional<std::string> env_from_process(int pid, std::string_view key) {
    if (pid <= 0 || key.empty()) {
        return std::nullopt;
    }
    std::ifstream in("/proc/" + std::to_string(pid) + "/environ");
    if (!in) {
        return std::nullopt;
    }
    std::string blob((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const std::string prefix = std::string(key) + "=";
    std::size_t pos = 0;
    while (pos < blob.size()) {
        const auto end = blob.find('\0', pos);
        const auto entry = blob.substr(
            pos, end == std::string::npos ? std::string::npos : end - pos);
        if (entry.rfind(prefix, 0) == 0) {
            auto value = entry.substr(prefix.size());
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

std::optional<std::string> display_from_process_environ(int pid) {
    return env_from_process(pid, "DISPLAY");
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

std::optional<int> parse_display_number(const std::string& display) {
    const auto normalized = normalize_display_name(display);
    const auto colon = normalized.rfind(':');
    if (colon == std::string::npos || colon + 1 >= normalized.size()) {
        return std::nullopt;
    }
    try {
        return std::stoi(normalized.substr(colon + 1));
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<int> display_number_from_x11_unix_path(std::string_view path) {
    // /tmp/.X11-unix/XN or @/tmp/.X11-unix/XN (abstract)
    constexpr std::string_view kPrefix = "/tmp/.X11-unix/X";
    if (!path.empty() && path.front() == '@') {
        path.remove_prefix(1);
    }
    if (path.size() <= kPrefix.size() || path.compare(0, kPrefix.size(), kPrefix) != 0) {
        return std::nullopt;
    }
    const auto suffix = path.substr(kPrefix.size());
    if (suffix.empty() || suffix.find_first_not_of("0123456789") != std::string_view::npos) {
        return std::nullopt;
    }
    try {
        return std::stoi(std::string(suffix));
    } catch (...) {
        return std::nullopt;
    }
}

/** inode → nested display number for local X11 unix listeners. */
std::unordered_map<unsigned long, int> x11_listen_inode_to_display() {
    std::unordered_map<unsigned long, int> out;
    std::ifstream in("/proc/net/unix");
    if (!in) {
        return out;
    }
    std::string line;
    std::getline(in, line); // header
    while (std::getline(in, line)) {
        std::istringstream iss(line);
        std::string num;
        std::string ref;
        std::string proto;
        std::string flags;
        std::string type;
        std::string st;
        std::string inode_str;
        if (!(iss >> num >> ref >> proto >> flags >> type >> st >> inode_str)) {
            continue;
        }
        std::string path;
        std::getline(iss >> std::ws, path);
        const auto display_num = display_number_from_x11_unix_path(path);
        if (!display_num.has_value()) {
            continue;
        }
        try {
            out.emplace(std::stoul(inode_str), *display_num);
        } catch (...) {
            // ignore malformed inode
        }
    }
    return out;
}

std::unordered_set<unsigned long> process_socket_inodes(int pid) {
    std::unordered_set<unsigned long> out;
    if (pid <= 0) {
        return out;
    }
    std::error_code error;
    const auto fd_dir = std::filesystem::path("/proc") / std::to_string(pid) / "fd";
    for (const auto& entry : std::filesystem::directory_iterator(fd_dir, error)) {
        if (error) {
            break;
        }
        char target[256];
        const ssize_t n = ::readlink(entry.path().c_str(), target, sizeof(target) - 1);
        if (n <= 0) {
            continue;
        }
        target[n] = '\0';
        constexpr std::string_view kSock = "socket:[";
        std::string_view view(target);
        if (view.size() < kSock.size() + 2 || view.compare(0, kSock.size(), kSock) != 0
            || view.back() != ']') {
            continue;
        }
        view.remove_prefix(kSock.size());
        view.remove_suffix(1);
        try {
            out.insert(std::stoul(std::string(view)));
        } catch (...) {
            // ignore
        }
    }
    return out;
}

std::vector<int> descendant_pids_of(int root_pid) {
    std::vector<int> out;
    if (root_pid <= 0) {
        return out;
    }
    std::unordered_set<int> seen;
    std::queue<int> pending;
    pending.push(root_pid);
    seen.insert(root_pid);
    while (!pending.empty()) {
        const int parent = pending.front();
        pending.pop();
        for (const int child : child_pids_of(parent)) {
            if (!seen.insert(child).second) {
                continue;
            }
            out.push_back(child);
            pending.push(child);
        }
    }
    return out;
}

std::vector<int> owner_tree_pids(int owner_pid) {
    std::vector<int> pids = descendant_pids_of(owner_pid);
    pids.push_back(owner_pid);
    return pids;
}

std::optional<std::string> session_id_from_process_tree(int owner_pid) {
    if (owner_pid <= 0) {
        return std::nullopt;
    }
    for (const int pid : owner_tree_pids(owner_pid)) {
        if (const auto id = env_from_process(pid, kArchstreamerSessionIdEnv); id) {
            return id;
        }
    }
    return std::nullopt;
}

std::mutex g_xtest_session_mu;
std::unordered_map<std::string, std::string> g_xtest_session_displays;

std::optional<int> display_number_from_xwayland_cmdline(int pid) {
    if (pid <= 0) {
        return std::nullopt;
    }
    std::ifstream in("/proc/" + std::to_string(pid) + "/cmdline");
    if (!in) {
        return std::nullopt;
    }
    std::string blob((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<std::string> args;
    std::size_t pos = 0;
    while (pos < blob.size()) {
        const auto end = blob.find('\0', pos);
        args.push_back(blob.substr(
            pos, end == std::string::npos ? std::string::npos : end - pos));
        if (end == std::string::npos) {
            break;
        }
        pos = end + 1;
    }
    if (args.empty()) {
        return std::nullopt;
    }
    const auto base = std::filesystem::path(args[0]).filename().string();
    if (base.find("Xwayland") == std::string::npos && base.find("Xorg") == std::string::npos) {
        return std::nullopt;
    }
    for (std::size_t i = 1; i < args.size(); ++i) {
        if (args[i].size() >= 2 && args[i].front() == ':') {
            try {
                return std::stoi(args[i].substr(1));
            } catch (...) {
                return std::nullopt;
            }
        }
    }
    return std::nullopt;
}

/**
 * gamescope keeps the nested X11 listen fds even when Xwayland is reparented to a
 * subreaper — so ownership is "does this session tree hold the XN socket?", not
 * "is Xwayland a child of gamescope?".
 */
bool process_tree_holds_display(int owner_pid, int display_num) {
    if (owner_pid <= 0) {
        return false;
    }
    const auto inode_map = x11_listen_inode_to_display();
    std::unordered_set<unsigned long> target;
    for (const auto& [inode, number] : inode_map) {
        if (number == display_num) {
            target.insert(inode);
        }
    }
    for (const int pid : owner_tree_pids(owner_pid)) {
        if (!target.empty()) {
            const auto held = process_socket_inodes(pid);
            for (const unsigned long inode : target) {
                if (held.contains(inode)) {
                    return true;
                }
            }
        }
        if (const auto from_cmd = display_number_from_xwayland_cmdline(pid); from_cmd) {
            if (*from_cmd == display_num) {
                return true;
            }
        }
    }
    return false;
}

std::vector<std::string> displays_held_by_process_tree(int owner_pid) {
    std::vector<std::string> names;
    if (owner_pid <= 0) {
        return names;
    }
    const auto inode_map = x11_listen_inode_to_display();
    std::unordered_set<int> seen_display;
    auto add_display = [&](int display_num) {
        if (display_num < 0 || !seen_display.insert(display_num).second) {
            return;
        }
        names.push_back(":" + std::to_string(display_num));
    };
    for (const int pid : owner_tree_pids(owner_pid)) {
        for (const unsigned long inode : process_socket_inodes(pid)) {
            const auto it = inode_map.find(inode);
            if (it != inode_map.end()) {
                add_display(it->second);
            }
        }
        // firejail: listen fds may be netns-local and missing from host
        // /proc/net/unix until/unless a filesystem socket is published. Xwayland's
        // cmdline still reports ":N" for the nested server we should open.
        if (const auto from_cmd = display_number_from_xwayland_cmdline(pid); from_cmd) {
            add_display(*from_cmd);
        }
    }
    return names;
}

} // namespace

void register_session_xtest_display(const std::string& session_id, const std::string& display) {
    if (session_id.empty() || display.empty()) {
        return;
    }
    std::lock_guard lock(g_xtest_session_mu);
    g_xtest_session_displays[session_id] = display;
}

void register_session_xtest_display_for_owner(int owner_pid, const std::string& display) {
    if (owner_pid <= 0 || display.empty()) {
        return;
    }
    if (const auto session_id = session_id_from_process_tree(owner_pid); session_id) {
        register_session_xtest_display(*session_id, display);
    }
}

void unregister_session_xtest_display(const std::string& session_id) {
    if (session_id.empty()) {
        return;
    }
    std::lock_guard lock(g_xtest_session_mu);
    g_xtest_session_displays.erase(session_id);
}

std::optional<std::string> lookup_session_xtest_display(const std::string& session_id) {
    if (session_id.empty()) {
        return std::nullopt;
    }
    std::lock_guard lock(g_xtest_session_mu);
    const auto it = g_xtest_session_displays.find(session_id);
    if (it == g_xtest_session_displays.end()) {
        return std::nullopt;
    }
    return it->second;
}

bool display_belongs_to_process_tree(const std::string& display, int owner_pid) {
    if (owner_pid <= 0 || display.empty() || is_host_desktop_display_name(display)) {
        return false;
    }
    const auto display_num = parse_display_number(display);
    if (!display_num.has_value()) {
        return false;
    }
    // Socket ownership is ground truth. The session lease is only a pin hint —
    // if reservation missed and Xwayland landed elsewhere, trust the fds.
    if (process_tree_holds_display(owner_pid, *display_num)) {
        return true;
    }
    if (const auto session_id = session_id_from_process_tree(owner_pid); session_id) {
        if (const auto leased = lookup_session_xtest_display(*session_id); leased) {
            return normalize_display_name(*leased) == normalize_display_name(display);
        }
    }
    return false;
}

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
        // 1) Session id → leased nested display on the host.
        if (const auto session_id = session_id_from_process_tree(owner_pid); session_id) {
            if (const auto leased = lookup_session_xtest_display(*session_id); leased) {
                add(*leased);
            }
        }
        // 2) Host-known preferred (same lease, passed at plug time).
        add(preferred);
        // 3) Socket fds still held by this tree (fallback).
        for (const auto& name : displays_held_by_process_tree(owner_pid)) {
            add(name);
        }
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

        const auto dismiss_error_applets = [&]() {
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
            for (const auto& name : displays) {
                try_dismiss_error_applet_on_display(name);
            }
        };

        // Wait until a dialog is mapped *and* holding keyboard focus (wants typed text),
        // not merely present unmapped in the window tree (boot-time false positives).
        // False once the session drops the bridge.
        // Also services manual pad-OSK injects so the escape hatch works while idle.
        // While idle, auto-OK Ryujinx Error applet dialogs (link cancelled, etc.).
        const auto wait_for_dialog = [&](std::string& out) {
            for (int attempt = 0;; ++attempt) {
                if (weak_bridge.expired()) {
                    return false;
                }
                if (drain_manual_inject()) {
                    // Inject may have dismissed the dialog; keep waiting for the next one.
                    continue;
                }
                dismiss_error_applets();
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
            std::string request_prompt = prompt;
            // Prefer the HeaderText Ryujinx paints on the ContentDialog (OCR), not a
            // hardcoded "What is your name?" — games ask for codes, nicknames, etc.
            {
                install_x_error_guard();
                Display* display = XOpenDisplay(display_name.c_str());
                if (display != nullptr) {
                    install_x_io_exit_guard(display);
                    Window dialog = 0;
                    if (const auto focused = find_focused_text_dialog(display);
                        focused.has_value()) {
                        dialog = *focused;
                    } else if (
                        const auto any =
                            find_viewable_titled(display, is_soft_keyboard_dialog_title);
                        any.has_value()) {
                        dialog = *any;
                    }
                    XCloseDisplay(display);
                    if (dialog != 0) {
                        if (auto ocr = ocr_soft_keyboard_prompt_on_display(
                                display_name, dialog);
                            !ocr.empty()) {
                            request_prompt = std::move(ocr);
                            std::cout
                                << "Ryujinx Software Keyboard: OCR prompt \""
                                << request_prompt << "\"\n";
                        }
                    }
                }
            }

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
                    request = bridge->make_request(request_prompt, /*initial_text=*/{}, 12);
                }
                bridge->publish_request(request);
            }
            std::cout
                << "Ryujinx Software Keyboard: focused text dialog on " << display_name
                << " — requesting pad OSK (id=" << request.request_id
                << ", prompt=\"" << request_prompt << "\")\n";

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
