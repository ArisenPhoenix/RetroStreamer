#include "client/remoted_keyboard_source.hpp"
#include "client/remoted_keyboard_sources.hpp"

#include <atomic>
#include <utility>

namespace archstreamer {
namespace {

std::atomic<std::uint32_t> g_gui_focus_keys{0};

} // namespace

CompositeRemotedKeyboardSource::CompositeRemotedKeyboardSource(
    std::vector<std::shared_ptr<RemotedKeyboardSource>> sources)
    : sources_(std::move(sources)) {
}

std::string_view CompositeRemotedKeyboardSource::name() const {
    return "composite";
}

bool CompositeRemotedKeyboardSource::available() const {
    for (const auto& source : sources_) {
        if (source && source->available()) {
            return true;
        }
    }
    return false;
}

std::uint32_t CompositeRemotedKeyboardSource::poll_keys() {
    std::uint32_t keys = 0;
    for (const auto& source : sources_) {
        if (source) {
            keys |= source->poll_keys();
        }
    }
    return keys;
}

std::string CompositeRemotedKeyboardSource::status_detail() const {
    std::string detail;
    for (const auto& source : sources_) {
        if (!source) {
            continue;
        }
        if (!detail.empty()) {
            detail.push_back(' ');
        }
        detail += source->status_detail();
    }
    if (detail.empty()) {
        return "none";
    }
    return detail;
}

GuiFocusRemotedKeyboardSource& GuiFocusRemotedKeyboardSource::instance() {
    static GuiFocusRemotedKeyboardSource source;
    return source;
}

std::string_view GuiFocusRemotedKeyboardSource::name() const {
    return "gui-focus";
}

bool GuiFocusRemotedKeyboardSource::available() const {
    return true;
}

std::uint32_t GuiFocusRemotedKeyboardSource::poll_keys() {
    return g_gui_focus_keys.load(std::memory_order_relaxed);
}

std::string GuiFocusRemotedKeyboardSource::status_detail() const {
    return "gui-focus=yes";
}

void GuiFocusRemotedKeyboardSource::set_keys(std::uint32_t keys) {
    g_gui_focus_keys.store(keys, std::memory_order_relaxed);
}

void GuiFocusRemotedKeyboardSource::set_key_down(RemotedKey key, bool down) {
    const auto bit = static_cast<std::uint32_t>(key);
    auto keys = g_gui_focus_keys.load(std::memory_order_relaxed);
    if (down) {
        keys |= bit;
    } else {
        keys &= ~bit;
    }
    g_gui_focus_keys.store(keys, std::memory_order_relaxed);
}

std::uint32_t remoted_key_bit_from_qt_key(int qt_key) {
    switch (qt_key) {
    case 0x01000000: // Qt::Key_Escape
        return KeyEscape;
    case 0x01000001: // Qt::Key_Tab
        return KeyTab;
    case 0x01000003: // Qt::Key_Backspace
        return KeyBackspace;
    case 0x01000004: // Qt::Key_Return
    case 0x01000005: // Qt::Key_Enter
        return KeyEnter;
    case 0x01000012: // Qt::Key_Left
        return KeyLeft;
    case 0x01000013: // Qt::Key_Up
        return KeyUp;
    case 0x01000014: // Qt::Key_Right
        return KeyRight;
    case 0x01000015: // Qt::Key_Down
        return KeyDown;
    case 0x20: // Qt::Key_Space
        return KeySpace;
    case 0x50: // Qt::Key_P
        return KeyP;
    case 0x01000030: // Qt::Key_F1
        return KeyF1;
    case 0x01000037: // Qt::Key_F8
        return KeyF8;
    default:
        return 0;
    }
}

std::unique_ptr<RemotedKeyboardSource> make_default_remoted_keyboard_source() {
    std::vector<std::shared_ptr<RemotedKeyboardSource>> sources;
#if defined(_WIN32)
    sources.push_back(make_win32_remoted_keyboard_source());
#else
    // Primary: focus-independent /dev/input (works while gst video has focus).
    sources.push_back(make_evdev_remoted_keyboard_source());
#endif
    // Shared with the GUI event filter (lobby focus).
    sources.push_back(
        std::shared_ptr<RemotedKeyboardSource>(
            &GuiFocusRemotedKeyboardSource::instance(),
            [](RemotedKeyboardSource*) {}));
#if !defined(_WIN32)
    // Last resort for pure X11 clients (SPICE VMs). Not used on Wayland gtksink focus.
    sources.push_back(make_x11_keymap_remoted_keyboard_source());
#endif
    return std::make_unique<CompositeRemotedKeyboardSource>(std::move(sources));
}

} // namespace archstreamer
