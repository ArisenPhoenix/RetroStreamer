#pragma once

#include "common/keyboard_state.hpp"

#include <cstdint>
#include <string>

namespace archstreamer {

// How a remoted key is applied on the host.
enum class RemotedKeyAction : std::uint8_t {
    // Reserved / unused bit.
    Ignored = 0,
    // Fire a RetroArch network_cmd on every press *and* release edge.
    NetcmdEdgeToggle,
    // Fire a RetroArch network_cmd once on press.
    NetcmdPress,
    // Hold/release a real key on the capture X display via XTest (Space FF, arrows, …).
    XTestHold,
};

struct RemotedKeyBinding {
    RemotedKey key = KeySpace;
    RemotedKeyAction action = RemotedKeyAction::Ignored;
    // RetroArch NCI command name when action is Netcmd*.
    const char* netcmd = nullptr;
};

// Default kid-oriented bindings: Space = hold-like FF, F1 = menu, rest = XTest.
const RemotedKeyBinding* default_remoted_key_bindings(std::size_t& count);

// Applies remoted KeyboardState to RetroArch (netcmd + XTest on the capture display).
class VirtualKeyboard {
public:
    explicit VirtualKeyboard(std::string capture_display = {});
    ~VirtualKeyboard();

    VirtualKeyboard(const VirtualKeyboard&) = delete;
    VirtualKeyboard& operator=(const VirtualKeyboard&) = delete;

    void plug();
    void unplug();
    bool plugged() const { return plugged_; }

    void apply(const KeyboardState& state);
    void release_all();

private:
    void apply_xtest_edges(std::uint32_t previous, std::uint32_t next);
    void ensure_xtest_display();

    std::string capture_display_;
    void* display_ = nullptr; // Display*
    bool plugged_ = false;
    bool has_last_ = false;
    bool logged_ff_ = false;
    std::uint32_t last_keys_ = 0;
};

#ifndef _WIN32
// Ryujinx shows an Avalonia "Software Keyboard" ContentDialog for Switch swkbd
// (e.g. Pokemon "What is your name?"). Under gamescope that dialog is in the
// stream but not reachable with a pad — watch nested X displays and type `text`.
void schedule_ryujinx_name_dialog_autofill(std::string text);
#endif

} // namespace archstreamer
