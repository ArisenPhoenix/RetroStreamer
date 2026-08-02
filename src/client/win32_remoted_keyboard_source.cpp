#include "client/remoted_keyboard_sources.hpp"

#include "common/keyboard_state.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace archstreamer {
namespace {

bool win_down(int vk) {
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

class Win32RemotedKeyboardSource final : public RemotedKeyboardSource {
public:
    std::string_view name() const override {
        return "win32";
    }

    bool available() const override {
        return true;
    }

    std::uint32_t poll_keys() override {
        std::uint32_t keys = 0;
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
        if (win_down(VK_F8)) {
            keys |= KeyF8;
        }
        if (win_down('P')) {
            keys |= KeyP;
        }
        return keys;
    }

    std::string status_detail() const override {
        return "win32=yes";
    }
};

} // namespace

std::shared_ptr<RemotedKeyboardSource> make_win32_remoted_keyboard_source() {
    return std::make_shared<Win32RemotedKeyboardSource>();
}

} // namespace archstreamer
