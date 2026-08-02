#pragma once

#include "common/keyboard_state.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace archstreamer {

struct SoftKeyboardHostBridge;

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

    /** Switch capture DISPLAY (e.g. gamescope nested Xwayland) and unplug first. */
    void rebind_display(std::string capture_display);
    const std::string& capture_display() const { return capture_display_; }

    void apply(const KeyboardState& state);
    void release_all();

private:
    void apply_xtest_edges(std::uint32_t previous, std::uint32_t next);
    void apply_xtest_space_autorepeat();
    void ensure_xtest_display();

    std::string capture_display_;
    void* display_ = nullptr; // Display*
    bool plugged_ = false;
    bool has_last_ = false;
    bool logged_ff_ = false;
    std::uint32_t last_keys_ = 0;
    std::chrono::steady_clock::time_point last_space_repeat_{};
};

#ifndef _WIN32
// Soft-keyboard watcher for backends that show an on-screen text dialog
// (currently Ryujinx Avalonia under gamescope): wait until a dialog window is
// mapped and holding keyboard focus, ask the client pad OSK via
// SoftKeyboardHostBridge, then XTest-type the result.
// Falls back to `fallback_text` if no client responds in time.
// `preferred_display` is tried first (session capture / nested Xwayland).
void schedule_soft_keyboard(
    std::shared_ptr<SoftKeyboardHostBridge> bridge,
    std::string fallback_text,
    std::string prompt = "The game is asking for text. Enter it with the pad.",
    std::string preferred_display = {});

/** Preferred + common gamescope/Xvfb display names for XTest / soft-kbd probes. */
std::vector<std::string> xtest_display_candidates(const std::string& preferred = {});
#endif

/**
 * Platform-neutral entry for session launch paths. Creates `bridge` when null,
 * then schedules the Linux soft-keyboard watcher. No-op on Windows.
 * Call only when the active backend sets enable_soft_keyboard.
 */
void ensure_soft_keyboard(
    std::shared_ptr<SoftKeyboardHostBridge>& bridge,
    std::string fallback_text,
    std::string prompt = "The game is asking for text. Enter it with the pad.",
    std::string preferred_display = {});

} // namespace archstreamer
