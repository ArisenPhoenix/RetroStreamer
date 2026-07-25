#include "client/keyboard_poller.hpp"

#include "common/time.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <vector>

#include <dirent.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <unistd.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <X11/Xlib.h>
#include <X11/keysym.h>
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
    // Prefer real keyboards; skip pure mice/joysticks.
    return device_has_key(fd, KEY_SPACE) && device_has_key(fd, KEY_ENTER) &&
        device_has_key(fd, KEY_P);
}
#endif

} // namespace

struct KeyboardPoller::Impl {
#if defined(_WIN32)
#else
    Display* display = nullptr;
    std::vector<int> evdev_fds;
#endif
};

KeyboardPoller::KeyboardPoller() {
    impl_ = new Impl();
#if !defined(_WIN32)
    impl_->display = XOpenDisplay(nullptr);

    // Evdev is focus-independent: Space works while the gst video window is focused.
    // User must be in the `input` group (archstreamer VM client already is).
    if (DIR* dir = opendir("/dev/input")) {
        while (dirent* entry = readdir(dir)) {
            if (std::strncmp(entry->d_name, "event", 5) != 0) {
                continue;
            }
            const std::string path = std::string("/dev/input/") + entry->d_name;
            const int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
            if (fd < 0) {
                continue;
            }
            if (!looks_like_keyboard(fd)) {
                close(fd);
                continue;
            }
            impl_->evdev_fds.push_back(fd);
        }
        closedir(dir);
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
    // Evdev: drain the queue then sample EVIOCGKEY so we never miss a tap
    // (Space/F1 worked via events; letter keys were easy to miss under SPICE).
    for (int fd : impl_->evdev_fds) {
        input_event event{};
        while (true) {
            const ssize_t n = read(fd, &event, sizeof(event));
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;
                }
                break;
            }
            if (n != static_cast<ssize_t>(sizeof(event))) {
                break;
            }
        }
        unsigned char key_state[(KEY_MAX + 7) / 8] = {};
        if (ioctl(fd, EVIOCGKEY(sizeof(key_state)), key_state) == 0) {
            keys |= remoted_keys_from_evdev_state(key_state);
        }
    }

    // X11 keymap as a fallback (helps when evdev open failed).
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

    if (impl_->evdev_fds.empty() && impl_->display == nullptr) {
        return std::nullopt;
    }
#endif

    KeyboardState state;
    state.sequence = ++sequence_;
    state.timestamp_us = steady_timestamp_us();
    state.keys = keys;
    return state;
}

} // namespace archstreamer
