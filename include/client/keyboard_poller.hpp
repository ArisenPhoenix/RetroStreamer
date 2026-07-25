#pragma once

#include "common/keyboard_state.hpp"
#include "common/protocol.hpp"

#include <cstdint>
#include <optional>

namespace archstreamer {

// Poll a small fixed key set without requiring the video window to have focus
// (kids watch gst sinks; lobby Qt windows often do not own keyboard focus).
class KeyboardPoller {
public:
    KeyboardPoller();
    ~KeyboardPoller();

    KeyboardPoller(const KeyboardPoller&) = delete;
    KeyboardPoller& operator=(const KeyboardPoller&) = delete;

    // Returns nullopt when the platform poller could not start (still safe to ignore).
    std::optional<KeyboardState> poll();

private:
    struct Impl;
    Impl* impl_ = nullptr;
    std::uint32_t sequence_ = 0;
};

} // namespace archstreamer
