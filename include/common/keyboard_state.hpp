#pragma once

#include <cstdint>

namespace archstreamer {

// Fixed remoted key set (no modifiers / chords). Bits are stable on the wire.
enum RemotedKey : std::uint32_t {
    KeySpace = 1u << 0,
    KeyUp = 1u << 1,
    KeyDown = 1u << 2,
    KeyLeft = 1u << 3,
    KeyRight = 1u << 4,
    KeyEnter = 1u << 5,
    KeyEscape = 1u << 6,
    KeyTab = 1u << 7,
    KeyBackspace = 1u << 8,
    KeyF1 = 1u << 9,
    KeyP = 1u << 10,
    KeyF8 = 1u << 11,
};

struct KeyboardState {
    std::uint32_t sequence = 0;
    std::uint64_t timestamp_us = 0;
    std::uint32_t keys = 0;
};

inline bool key_down(const KeyboardState& state, RemotedKey key) {
    return (state.keys & static_cast<std::uint32_t>(key)) != 0;
}

inline bool same_keys(const KeyboardState& a, const KeyboardState& b) {
    return a.keys == b.keys;
}

} // namespace archstreamer
