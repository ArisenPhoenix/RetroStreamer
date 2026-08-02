#include "client/remoted_keyboard_sources.hpp"

#include "common/keyboard_state.hpp"

#include <cstdlib>
#include <string>

#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <unistd.h>

namespace archstreamer {
namespace {

bool x_key_down(Display* display, KeySym sym, char keys[32]) {
    const KeyCode code = XKeysymToKeycode(display, sym);
    if (code == 0) {
        return false;
    }
    return (keys[code / 8] & (1 << (code % 8))) != 0;
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

// Last-resort backend for pure X11 (e.g. SPICE VMs) when an X11 window has focus.
// Native Wayland video sinks (gtksink) do not update this keymap.
class X11KeymapRemotedKeyboardSource final : public RemotedKeyboardSource {
public:
    X11KeymapRemotedKeyboardSource()
        : display_(open_x11_display()) {
    }

    ~X11KeymapRemotedKeyboardSource() override {
        if (display_ != nullptr) {
            XCloseDisplay(display_);
        }
    }

    std::string_view name() const override {
        return "x11-keymap";
    }

    bool available() const override {
        return display_ != nullptr;
    }

    std::uint32_t poll_keys() override {
        if (display_ == nullptr) {
            return 0;
        }
        char keymap[32] = {};
        XQueryKeymap(display_, keymap);
        auto down = [&](KeySym sym) { return x_key_down(display_, sym, keymap); };
        std::uint32_t keys = 0;
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
        if (down(XK_F8)) {
            keys |= KeyF8;
        }
        if (down(XK_p) || down(XK_P)) {
            keys |= KeyP;
        }
        return keys;
    }

    std::string status_detail() const override {
        return display_ != nullptr ? "x11=yes" : "x11=no";
    }

private:
    Display* display_ = nullptr;
};

} // namespace

std::shared_ptr<RemotedKeyboardSource> make_x11_keymap_remoted_keyboard_source() {
    return std::make_shared<X11KeymapRemotedKeyboardSource>();
}

} // namespace archstreamer
