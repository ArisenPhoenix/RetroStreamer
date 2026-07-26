#include "client/remoted_keyboard_bridge.hpp"

#include <atomic>

namespace archstreamer {
namespace {

std::atomic<std::uint32_t> g_qt_remoted_keys{0};

} // namespace

void remoted_keyboard_set_qt_keys(std::uint32_t keys) {
    g_qt_remoted_keys.store(keys, std::memory_order_relaxed);
}

std::uint32_t remoted_keyboard_qt_keys() {
    return g_qt_remoted_keys.load(std::memory_order_relaxed);
}

std::uint32_t remoted_key_bit_from_qt_key(int qt_key) {
    // Qt::Key_* values — keep numeric to avoid Qt headers in the client lib.
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
    default:
        return 0;
    }
}

} // namespace archstreamer
