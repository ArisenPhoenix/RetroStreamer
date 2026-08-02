#include "client/remoted_keyboard_sources.hpp"

#include "common/keyboard_state.hpp"

#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <string>
#include <unordered_set>
#include <vector>

#include <dirent.h>
#include <limits.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace archstreamer {
namespace {

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
    if (key_bit_set(bits, KEY_F8)) {
        keys |= KeyF8;
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
    case KEY_F8:
        return KeyF8;
    case KEY_P:
        return KeyP;
    default:
        return 0;
    }
}

void append_keyboard_log(const std::string& line) {
    ::mkdir("/tmp/archstreamer-logs", 0755);
    std::ofstream out("/tmp/archstreamer-logs/keyboard.log", std::ios::app);
    if (out) {
        out << line << '\n';
    }
}

class EvdevRemotedKeyboardSource final : public RemotedKeyboardSource {
public:
    EvdevRemotedKeyboardSource() {
        scan();
        append_keyboard_log(
            "evdev ctor keyboards=" + std::to_string(evdev_fds_.size()) +
            " opened_nodes=" + std::to_string(opened_nodes_));
    }

    ~EvdevRemotedKeyboardSource() override {
        for (int fd : evdev_fds_) {
            if (fd >= 0) {
                close(fd);
            }
        }
    }

    std::string_view name() const override {
        return "evdev";
    }

    bool available() const override {
        return !evdev_fds_.empty();
    }

    std::uint32_t poll_keys() override {
        if (evdev_fds_.empty()) {
            if (++rescan_ticks_ >= 125) {
                rescan_ticks_ = 0;
                scan();
            }
        }

        std::uint32_t keys = 0;
        for (int fd : evdev_fds_) {
            input_event event{};
            while (true) {
                const ssize_t n = read(fd, &event, sizeof(event));
                if (n < 0 || n != static_cast<ssize_t>(sizeof(event))) {
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
                    event_keys_ &= ~bit;
                } else {
                    event_keys_ |= bit;
                }
            }
            unsigned char key_state[(KEY_MAX + 7) / 8] = {};
            if (ioctl(fd, EVIOCGKEY(sizeof(key_state)), key_state) == 0) {
                keys |= remoted_keys_from_evdev_state(key_state);
            }
        }
        keys |= event_keys_;
        return keys;
    }

    std::string status_detail() const override {
        return "evdev=" + std::to_string(evdev_fds_.size());
    }

private:
    bool try_open_path(const std::string& path) {
        char resolved[PATH_MAX] = {};
        const char* canonical = realpath(path.c_str(), resolved);
        const std::string key = canonical != nullptr ? canonical : path;
        if (evdev_paths_.count(key) != 0) {
            return false;
        }
        const int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) {
            return false;
        }
        ++opened_nodes_;
        if (!looks_like_keyboard(fd)) {
            close(fd);
            return false;
        }
        evdev_fds_.push_back(fd);
        evdev_paths_.insert(key);
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

    std::vector<int> evdev_fds_;
    std::unordered_set<std::string> evdev_paths_;
    std::uint32_t event_keys_ = 0;
    int opened_nodes_ = 0;
    int rescan_ticks_ = 0;
};

} // namespace

std::shared_ptr<RemotedKeyboardSource> make_evdev_remoted_keyboard_source() {
    return std::make_shared<EvdevRemotedKeyboardSource>();
}

} // namespace archstreamer
