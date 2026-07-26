#pragma once

#include "common/keyboard_state.hpp"

#include <cstdint>
#include <string>

namespace archstreamer {

// Process-wide remoted key bits. Qt GUI ORs focus-based keys here; KeyboardPoller
// merges them with evdev/X11 so Space still works when the lobby window is focused.
void remoted_keyboard_set_qt_keys(std::uint32_t keys);
std::uint32_t remoted_keyboard_qt_keys();

// Map a Qt key code (Qt::Key_*) onto RemotedKey bits. Returns 0 if unmapped.
std::uint32_t remoted_key_bit_from_qt_key(int qt_key);

} // namespace archstreamer
