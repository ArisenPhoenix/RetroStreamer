#pragma once

#include "common/keyboard_state.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace archstreamer {

// Client-side capture of remoted hotkeys (Space=FF, P=pause, …).
// The UDP KeyboardInput wire format is unchanged; this only samples local keys.
//
// Semantics (host InputRouter): remoted keys are session-wide — they do not
// require a pad seat. Pads remain seated.
class RemotedKeyboardSource {
public:
    virtual ~RemotedKeyboardSource() = default;

    virtual std::string_view name() const = 0;

    // True when this backend can contribute key bits (devices opened, etc.).
    virtual bool available() const = 0;

    // Current RemotedKey bit mask (held keys). Safe to call every input tick.
    virtual std::uint32_t poll_keys() = 0;

    // Short diagnostic fragment for logs (e.g. "evdev=2").
    virtual std::string status_detail() const = 0;
};

// ORs every child source each poll (evdev | gui-focus | …).
class CompositeRemotedKeyboardSource final : public RemotedKeyboardSource {
public:
    explicit CompositeRemotedKeyboardSource(
        std::vector<std::shared_ptr<RemotedKeyboardSource>> sources);

    std::string_view name() const override;
    bool available() const override;
    std::uint32_t poll_keys() override;
    std::string status_detail() const override;

    const std::vector<std::shared_ptr<RemotedKeyboardSource>>& sources() const {
        return sources_;
    }

private:
    std::vector<std::shared_ptr<RemotedKeyboardSource>> sources_;
};

// Lobby / Qt (or other GUI) focus path. Process-wide singleton so the GUI can
// push key edges without owning the ClientApp input thread.
class GuiFocusRemotedKeyboardSource final : public RemotedKeyboardSource {
public:
    static GuiFocusRemotedKeyboardSource& instance();

    std::string_view name() const override;
    bool available() const override;
    std::uint32_t poll_keys() override;
    std::string status_detail() const override;

    void set_keys(std::uint32_t keys);
    void set_key_down(RemotedKey key, bool down);

private:
    GuiFocusRemotedKeyboardSource() = default;
};

// Map a Qt::Key_* integer onto RemotedKey bits (no Qt headers in this lib).
std::uint32_t remoted_key_bit_from_qt_key(int qt_key);

// Linux: Evdev (primary) + GuiFocus + optional X11 keymap (last resort).
// Windows: Win32 GetAsyncKeyState + GuiFocus.
std::unique_ptr<RemotedKeyboardSource> make_default_remoted_keyboard_source();

} // namespace archstreamer
