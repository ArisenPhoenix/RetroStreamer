#pragma once

#include "common/keyboard_state.hpp"
#include "common/protocol.hpp"
#include "host/retroarch_netcmd.hpp"

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
    // Hold/release a real key on the capture X display via XTest (arrows, Enter, …).
    XTestHold,
};

struct RemotedKeyBinding {
    RemotedKey key = KeySpace;
    RemotedKeyAction action = RemotedKeyAction::Ignored;
    // RetroArch NCI command name when action is Netcmd*.
    const char* netcmd = nullptr;
};

// Default kid-oriented bindings: Space/P are desktop tap-toggles; rest = XTest.
const RemotedKeyBinding* default_remoted_key_bindings(std::size_t& count);

// Applies remoted KeyboardState / EmulatorControl to the running emulator.
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

    /** Emulator/gamecope leader PID — used to focus the right X window via _NET_WM_PID. */
    void set_target_pid(int pid) { target_pid_ = pid; }

    /**
     * Switch/Ryujinx: P via XTest F5; FF via F1 VSync cycle to Custom@200%.
     * melonDS: F5 pause toggle + Space hold-FF (Keyboard binds in melonDS.toml).
     * RetroArch: NCI pause + Space hold-FF.
     */
    void set_switch_style_hotkeys(bool enabled) { switch_style_hotkeys_ = enabled; }
    void set_melonds_style_hotkeys(bool enabled) { melonds_style_hotkeys_ = enabled; }

    /** Per-session RetroArch network_cmd_port (multi-slot hosts). */
    void set_netcmd_port(std::uint16_t port) { netcmd_port_ = port; }

    void apply(const KeyboardState& state);
    /**
     * Absolute pause / FF from EmulatorControl. Prefer EmulatorControlPlane as
     * the ingress; this remains for direct callers and remoted-key bridges.
     */
    void apply_emulator_control(const EmulatorControl& control);
    void set_paused(bool want_paused, bool force = false);
    void set_fast_forward(bool want_on, bool force = false);
    /** If FF is desired on, re-press the hold key (after unpause focus steal). */
    void reassert_fast_forward_hold();
    /** One-shot melonDS screen swap (keyboard F6); no-op on other backends. */
    void trigger_screen_swap();
    void release_all();

private:
    void apply_xtest_edges(std::uint32_t previous, std::uint32_t next);
    void apply_xtest_space_autorepeat();
    void ensure_xtest_display();
    bool focus_emulator_window(bool settle);
    void xtest_tap_keysym(unsigned long keysym);
    void xtest_set_keysym(unsigned long keysym, bool down);
    void set_ff_key_held(bool want_held);
    unsigned long ff_hold_keysym() const;

    std::string capture_display_;
    void* display_ = nullptr; // Display*
    bool plugged_ = false;
    bool has_last_ = false;
    bool logged_ff_ = false;
    bool logged_focus_miss_ = false;
    bool switch_style_hotkeys_ = false;
    bool melonds_style_hotkeys_ = false;
    bool paused_ = false;
    bool fast_forward_ = false;
    /** True while holding the FF key (Space for RetroArch hold-FF). */
    bool ff_key_held_ = false;
    /**
     * Ryujinx F1 VSync cycle index: 0=Switch, 1=Unbounded, 2=Custom@200%.
     * ryujinx_switch_vsync_ mirrors (mode==0) for older call sites.
     */
    std::uint8_t ryujinx_vsync_mode_ = 0;
    bool ryujinx_switch_vsync_ = true;
    int target_pid_ = 0;
    std::uint16_t netcmd_port_ = DefaultRetroArchNetcmdPort;
    std::uint32_t last_keys_ = 0;
    std::chrono::steady_clock::time_point last_space_repeat_{};
};

#ifndef _WIN32
// Soft-keyboard watcher for backends that show an on-screen text dialog
// (currently Ryujinx Avalonia under gamescope): wait until a dialog window is
// mapped and holding keyboard focus, ask the client pad OSK via
// SoftKeyboardHostBridge, then XTest-type the accepted text.
// Cancel / empty / timeout never invents a name — if the game dialog is still
// focused, another SoftKeyboardRequest is published; otherwise we wait for the
// next real dialog. `fallback_text` is unused (kept for call-site compatibility).
// `preferred_display` / `owner_pid` scope probes to this session's nested
// Xwayland — never scan sibling gamescope displays (concurrent slots).
#endif
// Declared on every platform; Windows provides no-op / SendInput backends in
// windows_soft_keyboard.cpp / windows_virtual_keyboard.cpp (Linux reference stays
// in virtual_keyboard.cpp).
void schedule_soft_keyboard(
    std::shared_ptr<SoftKeyboardHostBridge> bridge,
    std::string fallback_text,
    std::string prompt = "The game is asking for text. Enter it with the pad.",
    std::string preferred_display = {},
    int owner_pid = 0);

void ensure_soft_keyboard(
    std::shared_ptr<SoftKeyboardHostBridge>& bridge,
    std::string fallback_text,
    std::string prompt = "The game is asking for text. Enter it with the pad.",
    std::string preferred_display = {},
    int owner_pid = 0);

/** Preferred + common gamescope/Xvfb display names for XTest / soft-kbd probes. */
std::vector<std::string> xtest_display_candidates(const std::string& preferred = {});

/**
 * Displays this soft-keyboard watcher may probe. When owner_pid > 0, only
 * DISPLAYs from that gamescope/emulator process tree (plus preferred), never
 * a blanket scan of every local X socket.
 */
std::vector<std::string> soft_keyboard_display_candidates(
    const std::string& preferred = {},
    int owner_pid = 0);

} // namespace archstreamer
