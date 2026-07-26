#pragma once

#include "common/keyboard_state.hpp"
#include "common/protocol.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace archstreamer {

// Thin session helper: owns make_default_remoted_keyboard_source() and stamps
// sequence/timestamp onto KeyboardState for UDP. Capture backends live behind
// RemotedKeyboardSource (evdev primary, gui-focus, optional X11 keymap).
class KeyboardPoller {
public:
    KeyboardPoller();
    ~KeyboardPoller();

    KeyboardPoller(const KeyboardPoller&) = delete;
    KeyboardPoller& operator=(const KeyboardPoller&) = delete;

    std::optional<KeyboardState> poll();
    std::string backend_status() const;

private:
    struct Impl;
    Impl* impl_ = nullptr;
    std::uint32_t sequence_ = 0;
};

} // namespace archstreamer
