#include "client/keyboard_poller.hpp"

#include "client/remoted_keyboard_bridge.hpp"
#include "common/time.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <string>
#include <unordered_set>
#include <vector>

#include <dirent.h>
#include <limits.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <cstdlib>
#endif

namespace archstreamer {
namespace {

#if defined(_WIN32)
bool win_down(int vk) {
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}
#else
bool x_key_down(Display* display, KeySym sym, char keys[32]) {
    const KeyCode code = XKeysymToKeycode(display, sym);
    if (code == 0) {
        return false;
    }
    return (keys[code / 8] & (1 << (code % 8))) != 0;
}

bool key_bit_set(const unsigned char* bits, int key) {
    return (bits[key / 8] & (1u << (key % 8))) != 0;
}

std::uint32_t remoted_keys_from_evdev_state(const unsigned char* bits) {
    std::uint32_t keys = 0;
    if (key_bit_set(bits, KEY_SPACE)) {
        keys |= KeySpace;
    }
    if (key_bit_set(bits, KEY_UP)) {
        keys |= KeyUp;
    }
    if (key_bit_set(bits, KEY_DOWN)) {
        keys |= KeyDown;
    }
    if (key_bit_set(bits, KEY_LEFT)) {
        keys |= KeyLeft;
    }
    if (key_bit_set(bits, KEY_RIGHT)) {
        keys |= KeyRight;
    }
    if (key_bit_set(bits, KEY_ENTER) || key_bit_set(bits, KEY_KPENTER)) {
        keys |= KeyEnter;
    }
    if (key_bit_set(bits, KEY_ESC)) {
        keys |= KeyEscape;
    }
    if (key_bit_set(bits, KEY_TAB)) {
        keys |= KeyTab;
    }
    if (key_bit_set(bits, KEY_BACKSPACE)) {
        keys |= KeyBackspace;
    }
    if (key_bit_set(bits, KEY_F1)) {
        keys |= KeyF1;
    }
    if (key_bit_set(bits, KEY_P)) {
        keys |= KeyP;
    }
    return keys;
}

bool device_has_key(int fd, int key) {
    static_assert(KEY_MAX < 768, "KEY_MAX unexpectedly large");
    unsigned char bits[(KEY_MAX + 7) / 8] = {};
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(bits)), bits) < 0) {
        return false;
    }
    return key_bit_set(bits, key);
}

bool looks_like_keyboard(int fd) {
    return device_has_key(fd, KEY_SPACE) &&
        (device_has_key(fd, KEY_ENTER) || device_has_key(fd, KEY_KPENTER)) &&
        (device_has_key(fd, KEY_P) || device_has_key(fd, KEY_A));
}

void ensure_log_dir() {
    ::mkdir("/tmp/archstreamer-logs", 0755);
}

void append_keyboard_log(const std::string& line) {
    ensure_log_dir();
    std::ofstream out("/tmp/archstreamer-logs/keyboard.log", std::ios::app);
    if (!out) {
        return;
    }
    out << line << '\n';
}

Display* open_x11_display() {
    if (Display* display = XOpenDisplay(nullptr); display != nullptr) {
        return display;
    }
    if (const char* existing = std::getenv("DISPLAY");
        existing == nullptr || existing[0] == '\0') {
        if (::access("/tmp/.X11-unix/X0", F_OK) == 0) {
            return XOpenDisplay(":0");
        }
    }
    return nullptr;
}

void warn_keyboard_unavailable(int opened_nodes, int keyboard_nodes) {
    static bool warned = false;
    if (warned) {
        return;
    }
    warned = true;
    std::cerr
        << "Warning: remoted keyboard unavailable (opened " << opened_nodes
        << " /dev/input node(s), " << keyboard_nodes << " keyboard-like). "
        << "On Flatpak/Wayland add the user to the 'input' group and ensure the "
        << "app has --device=all and --socket=x11.\n";
    append_keyboard_log(
        "unavailable opened=" + std::to_string(opened_nodes) +
        " keyboards=" + std::to_string(keyboard_nodes));
}

std::uint32_t bit_for_evdev_code(int code) {
    switch (code) {
    case KEY_SPACE:
        return KeySpace;
    case KEY_UP:
        return KeyUp;
    case KEY_DOWN:
        return KeyDown;
    case KEY_LEFT:
        return KeyLeft;
    case KEY_RIGHT:
        return KeyRight;
    case KEY_ENTER:
    case KEY_KPENTER:
        return KeyEnter;
    case KEY_ESC:
        return KeyEscape;
    case KEY_TAB:
        return KeyTab;
    case KEY_BACKSPACE:
        return KeyBackspace;
    case KEY_F1:
        return KeyF1;
    case KEY_P:
        return KeyP;
    default:
        return 0;
    }
}
#endif

} // namespace

struct KeyboardPoller::Impl {
#if !defined(_WIN32)
    Display* display = nullptr;
    std::vector<int> evdev_fds;
    std::unordered_set<std::string> evdev_paths;
    std::uint32_t event_keys = 0;
    int opened_nodes = 0;
    int keyboard_nodes = 0;
    int rescan_ticks = 0;

    bool try_open_path(const std::string& path) {
        char resolved[PATH_MAX] = {};
        const char* canonical = realpath(path.c_str(), resolved);
        const std::string key = canonical != nullptr ? canonical : path;
        if (evdev_paths.count(key) != 0) {
            return false;
        }
        const int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) {
            append_keyboard_log("open_fail " + path + " errno=" + std::to_string(errno));
            return false;
        }
        ++opened_nodes;
        if (!looks_like_keyboard(fd)) {
            close(fd);
            return false;
        }
        char name[256] = {};
        ioctl(fd, EVIOCGNAME(sizeof(name)), name);
        evdev_fds.push_back(fd);
        evdev_paths.insert(key);
        ++keyboard_nodes;
        append_keyboard_log(
            std::string("open_ok ") + path + " -> " + key + " name='" + name + "'");
        return true;
    }

    void scan() {
        if (DIR* dir = opendir("/dev/input/by-id")) {
            while (dirent* entry = readdir(dir)) {
                const char* name = entry->d_name;
                if (std::strstr(name, "event-kbd") == nullptr &&
                    std::strstr(name, "-kbd") == nullptr) {
                    continue;
                }
                try_open_path(std::string("/dev/input/by-id/") + name);
            }
            closedir(dir);
        }
        if (DIR* dir = opendir("/dev/input")) {
            while (dirent* entry = readdir(dir)) {
                if (std::strncmp(entry->d_name, "event", 5) != 0) {
                    continue;
                }
                try_open_path(std::string("/dev/input/") + entry->d_name);
            }
            closedir(dir);
        }
    }
#endif
};

KeyboardPoller::KeyboardPoller() {
    impl_ = new Impl();
#if !defined(_WIN32)
    append_keyboard_log("ctor begin");
    impl_->display = open_x11_display();
    append_keyboard_log(
        std::string("x11=") + (impl_->display != nullptr ? "yes" : "no") +
        " DISPLAY=" + (std::getenv("DISPLAY") ? std::getenv("DISPLAY") : "(null)") +
        " WAYLAND=" +
            (std::getenv("WAYLAND_DISPLAY") ? std::getenv("WAYLAND_DISPLAY") : "(null)"));
    impl_->scan();
    if (impl_->evdev_fds.empty()) {
        warn_keyboard_unavailable(impl_->opened_nodes, impl_->keyboard_nodes);
    } else {
        append_keyboard_log(
            "ready keyboards=" + std::to_string(impl_->evdev_fds.size()) +
            " opened_nodes=" + std::to_string(impl_->opened_nodes));
        std::cerr
            << "Remoted keyboard: watching " << impl_->evdev_fds.size()
            << " /dev/input keyboard(s)\n";
    }
#endif
}

KeyboardPoller::~KeyboardPoller() {
#if !defined(_WIN32)
    if (impl_ != nullptr) {
        for (int fd : impl_->evdev_fds) {
            if (fd >= 0) {
                close(fd);
            }
        }
        if (impl_->display != nullptr) {
            XCloseDisplay(impl_->display);
        }
    }
#endif
    delete impl_;
    impl_ = nullptr;
}

std::string KeyboardPoller::backend_status() const {
#if defined(_WIN32)
    return "Remoted keyboard: Win32 GetAsyncKeyState";
#else
    if (impl_ == nullptr) {
        return "Remoted keyboard: unavailable";
    }
    std::string status = "Remoted keyboard: evdev=" + std::to_string(impl_->evdev_fds.size());
    status += impl_->display != nullptr ? " x11=yes" : " x11=no";
    status += " qt_bridge=yes";
    return status;
#endif
}

std::optional<KeyboardState> KeyboardPoller::poll() {
    if (impl_ == nullptr) {
        return std::nullopt;
    }

    std::uint32_t keys = 0;
#if defined(_WIN32)
    if (win_down(VK_SPACE)) {
        keys |= KeySpace;
    }
    if (win_down(VK_UP)) {
        keys |= KeyUp;
    }
    if (win_down(VK_DOWN)) {
        keys |= KeyDown;
    }
    if (win_down(VK_LEFT)) {
        keys |= KeyLeft;
    }
    if (win_down(VK_RIGHT)) {
        keys |= KeyRight;
    }
    if (win_down(VK_RETURN)) {
        keys |= KeyEnter;
    }
    if (win_down(VK_ESCAPE)) {
        keys |= KeyEscape;
    }
    if (win_down(VK_TAB)) {
        keys |= KeyTab;
    }
    if (win_down(VK_BACK)) {
        keys |= KeyBackspace;
    }
    if (win_down(VK_F1)) {
        keys |= KeyF1;
    }
    if (win_down('P')) {
        keys |= KeyP;
    }
#else
    if (impl_->evdev_fds.empty()) {
        if (++impl_->rescan_ticks >= 125) {
            impl_->rescan_ticks = 0;
            const int before = static_cast<int>(impl_->evdev_fds.size());
            impl_->scan();
            if (static_cast<int>(impl_->evdev_fds.size()) > before) {
                append_keyboard_log(
                    "rescan found keyboards=" + std::to_string(impl_->evdev_fds.size()));
            }
        }
    }

    for (int fd : impl_->evdev_fds) {
        input_event event{};
        while (true) {
            const ssize_t n = read(fd, &event, sizeof(event));
            if (n < 0) {
                break;
            }
            if (n != static_cast<ssize_t>(sizeof(event))) {
                break;
            }
            if (event.type != EV_KEY) {
                continue;
            }
            const std::uint32_t bit = bit_for_evdev_code(event.code);
            if (bit == 0) {
                continue;
            }
            if (event.value == 0) {
                impl_->event_keys &= ~bit;
            } else {
                impl_->event_keys |= bit;
            }
        }
        unsigned char key_state[(KEY_MAX + 7) / 8] = {};
        if (ioctl(fd, EVIOCGKEY(sizeof(key_state)), key_state) == 0) {
            keys |= remoted_keys_from_evdev_state(key_state);
        }
    }
    keys |= impl_->event_keys;

    if (impl_->display != nullptr) {
        char keymap[32] = {};
        XQueryKeymap(impl_->display, keymap);
        auto down = [&](KeySym sym) { return x_key_down(impl_->display, sym, keymap); };
        if (down(XK_space)) {
            keys |= KeySpace;
        }
        if (down(XK_Up)) {
            keys |= KeyUp;
        }
        if (down(XK_Down)) {
            keys |= KeyDown;
        }
        if (down(XK_Left)) {
            keys |= KeyLeft;
        }
        if (down(XK_Right)) {
            keys |= KeyRight;
        }
        if (down(XK_Return) || down(XK_KP_Enter)) {
            keys |= KeyEnter;
        }
        if (down(XK_Escape)) {
            keys |= KeyEscape;
        }
        if (down(XK_Tab)) {
            keys |= KeyTab;
        }
        if (down(XK_BackSpace)) {
            keys |= KeyBackspace;
        }
        if (down(XK_F1)) {
            keys |= KeyF1;
        }
        if (down(XK_p) || down(XK_P)) {
            keys |= KeyP;
        }
    }

    keys |= remoted_keyboard_qt_keys();
#endif

    KeyboardState state;
    state.sequence = ++sequence_;
    state.timestamp_us = steady_timestamp_us();
    state.keys = keys;
    return state;
}

} // namespace archstreamer
