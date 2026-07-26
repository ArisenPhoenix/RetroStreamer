#pragma once

#include "client/remoted_keyboard_source.hpp"

#include <memory>

namespace archstreamer {

#if defined(_WIN32)
std::shared_ptr<RemotedKeyboardSource> make_win32_remoted_keyboard_source();
#else
std::shared_ptr<RemotedKeyboardSource> make_evdev_remoted_keyboard_source();
std::shared_ptr<RemotedKeyboardSource> make_x11_keymap_remoted_keyboard_source();
#endif

} // namespace archstreamer
