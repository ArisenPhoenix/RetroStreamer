#include "host/virtual_keyboard.hpp"

#include "host/retroarch_netcmd.hpp"

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <chrono>
#include <iostream>
#include <thread>

namespace archstreamer {
namespace {

constexpr RemotedKeyBinding kDefaultBindings[] = {
    {KeySpace, RemotedKeyAction::XTestHold, nullptr},
    {KeyP, RemotedKeyAction::NetcmdPress, nullptr},
    {KeyF1, RemotedKeyAction::NetcmdPress, "MENU_TOGGLE"},
    {KeyF8, RemotedKeyAction::XTestHold, nullptr},
    {KeyUp, RemotedKeyAction::XTestHold, nullptr},
    {KeyDown, RemotedKeyAction::XTestHold, nullptr},
    {KeyLeft, RemotedKeyAction::XTestHold, nullptr},
    {KeyRight, RemotedKeyAction::XTestHold, nullptr},
    {KeyEnter, RemotedKeyAction::XTestHold, nullptr},
    {KeyEscape, RemotedKeyAction::XTestHold, nullptr},
    {KeyTab, RemotedKeyAction::XTestHold, nullptr},
    {KeyBackspace, RemotedKeyAction::XTestHold, nullptr},
};

bool bit_down(std::uint32_t keys, RemotedKey key) {
    return (keys & static_cast<std::uint32_t>(key)) != 0;
}

WORD virtual_key_for_remoted(RemotedKey key) {
    switch (key) {
    case KeySpace:
        return VK_SPACE;
    case KeyP:
        return 'P';
    case KeyF1:
        return VK_F1;
    case KeyF8:
        return VK_F8;
    case KeyUp:
        return VK_UP;
    case KeyDown:
        return VK_DOWN;
    case KeyLeft:
        return VK_LEFT;
    case KeyRight:
        return VK_RIGHT;
    case KeyEnter:
        return VK_RETURN;
    case KeyEscape:
        return VK_ESCAPE;
    case KeyTab:
        return VK_TAB;
    case KeyBackspace:
        return VK_BACK;
    default:
        return 0;
    }
}

void send_vk(WORD vk, bool down) {
    if (vk == 0) {
        return;
    }
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = vk;
    input.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
    SendInput(1, &input, sizeof(INPUT));
}

void tap_vk(WORD vk) {
    send_vk(vk, true);
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    send_vk(vk, false);
}

} // namespace

const RemotedKeyBinding* default_remoted_key_bindings(std::size_t& count) {
    count = sizeof(kDefaultBindings) / sizeof(kDefaultBindings[0]);
    return kDefaultBindings;
}

VirtualKeyboard::VirtualKeyboard(std::string capture_display)
    : capture_display_(std::move(capture_display)) {}

VirtualKeyboard::~VirtualKeyboard() {
    release_all();
}

void VirtualKeyboard::rebind_display(std::string capture_display) {
    unplug();
    capture_display_ = std::move(capture_display);
}

void VirtualKeyboard::plug() {
    plugged_ = true;
    paused_ = false;
    fast_forward_ = false;
    ff_key_held_ = false;
    ryujinx_vsync_mode_ = 0;
    ryujinx_switch_vsync_ = true;
}

void VirtualKeyboard::unplug() {
    release_all();
    plugged_ = false;
    has_last_ = false;
}

void VirtualKeyboard::xtest_tap_keysym(unsigned long keysym) {
    (void)keysym;
}

void VirtualKeyboard::xtest_set_keysym(unsigned long, bool) {}

void VirtualKeyboard::ensure_xtest_display() {}

bool VirtualKeyboard::focus_emulator_window(bool) {
    return true;
}

void VirtualKeyboard::apply_xtest_edges(std::uint32_t previous, std::uint32_t next) {
    std::size_t count = 0;
    const auto* bindings = default_remoted_key_bindings(count);
    for (std::size_t i = 0; i < count; ++i) {
        const auto& binding = bindings[i];
        if (binding.action != RemotedKeyAction::XTestHold) {
            continue;
        }
        if (binding.key == KeySpace) {
            continue;
        }
        const bool was = bit_down(previous, binding.key);
        const bool now = bit_down(next, binding.key);
        if (was == now) {
            continue;
        }
        send_vk(virtual_key_for_remoted(binding.key), now);
    }
}

void VirtualKeyboard::apply_xtest_space_autorepeat() {}

unsigned long VirtualKeyboard::ff_hold_keysym() const {
    // Windows uses VK codes in set_ff_key_held; keysym unused.
    return 0;
}

void VirtualKeyboard::set_ff_key_held(bool want_held) {
    const WORD vk = VK_SPACE;
    if (want_held == ff_key_held_) {
        if (want_held) {
            send_vk(vk, true);
        }
        return;
    }
    send_vk(vk, want_held);
    ff_key_held_ = want_held;
}

void VirtualKeyboard::reassert_fast_forward_hold() {
    if (!plugged_ || !fast_forward_) {
        return;
    }
    if (switch_style_hotkeys_) {
        return;
    }
    set_ff_key_held(true);
}

void VirtualKeyboard::set_paused(bool want_paused, bool force) {
    if (!plugged_) {
        return;
    }
    // F5 is a toggle: tap only when desired state differs from cache. force must
    // not re-tap on a match (inverts). RetroArch can still absolute-set below.
    if (want_paused == paused_) {
        if (switch_style_hotkeys_ || melonds_style_hotkeys_) {
            return;
        }
        if (!force) {
            const auto current = query_retroarch_paused(netcmd_port_);
            if (current.has_value() && *current == want_paused) {
                return;
            }
            if (!current.has_value()) {
                return;
            }
        }
    }
    if (switch_style_hotkeys_ || melonds_style_hotkeys_) {
        tap_vk(VK_F5);
        paused_ = want_paused;
        std::cout
            << "EmulatorControl: pause=" << (want_paused ? "on" : "off")
            << " (SendInput F5" << (melonds_style_hotkeys_ ? "/melonDS" : "")
            << (force ? ", force" : "") << ")\n";
        return;
    }
    if (set_retroarch_paused(want_paused, netcmd_port_)) {
        paused_ = want_paused;
        std::cout
            << "EmulatorControl: pause=" << (want_paused ? "on" : "off")
            << " (netcmd " << netcmd_port_ << (force ? ", force" : "") << ")\n";
    } else if (want_paused) {
        if (send_retroarch_netcmd("PAUSE_TOGGLE", netcmd_port_)) {
            paused_ = true;
            std::cout << "EmulatorControl: pause=on via PAUSE_TOGGLE (status unknown)\n";
        }
    }
}

void VirtualKeyboard::set_fast_forward(bool want_on, bool force) {
    if (!plugged_) {
        return;
    }
    if (switch_style_hotkeys_) {
        // Never re-cycle F1 when cache already matches (force retries desync VSync).
        if (want_on == fast_forward_) {
            return;
        }
        if (want_on) {
            if (ryujinx_vsync_mode_ == 0) {
                tap_vk(VK_F1);
                std::this_thread::sleep_for(std::chrono::milliseconds(40));
                tap_vk(VK_F1);
                ryujinx_vsync_mode_ = 2;
            }
            ryujinx_switch_vsync_ = false;
            fast_forward_ = true;
        } else {
            int guard = 0;
            while (ryujinx_vsync_mode_ != 0 && guard < 2) {
                tap_vk(VK_F1);
                std::this_thread::sleep_for(std::chrono::milliseconds(40));
                ryujinx_vsync_mode_ = static_cast<std::uint8_t>((ryujinx_vsync_mode_ + 1) % 3);
                ++guard;
            }
            ryujinx_switch_vsync_ = (ryujinx_vsync_mode_ == 0);
            fast_forward_ = false;
        }
        std::cout
            << "EmulatorControl: fast_forward=" << (want_on ? "on" : "off")
            << " (SendInput F1 VSync mode=" << static_cast<int>(ryujinx_vsync_mode_)
            << (force ? ", force" : "") << ")\n";
        return;
    }
    const bool already = (want_on == fast_forward_ && want_on == ff_key_held_);
    if (already && !force) {
        return;
    }
    if (already && force && want_on) {
        set_ff_key_held(true);
        return;
    }
    set_ff_key_held(want_on);
    fast_forward_ = want_on;
    std::cout
        << "EmulatorControl: fast_forward=" << (want_on ? "on" : "off")
        << " (hold Space" << (melonds_style_hotkeys_ ? "/melonDS" : "")
        << (force ? ", force" : "") << ")\n";
}

void VirtualKeyboard::trigger_screen_swap() {
    if (!plugged_) {
        return;
    }
    if (!melonds_style_hotkeys_) {
        std::cerr << "EmulatorControl: screen_swap ignored (not melonDS)\n";
        return;
    }
    tap_vk(VK_F6);
    std::cout << "EmulatorControl: screen_swap (SendInput F6/melonDS)\n";
}

void VirtualKeyboard::trigger_pause_toggle() {
    if (!plugged_) {
        return;
    }
    if (switch_style_hotkeys_ || melonds_style_hotkeys_) {
        tap_vk(VK_F5);
        paused_ = !paused_;
        std::cout
            << "EmulatorControl: pause_toggle → " << (paused_ ? "on" : "off")
            << " (SendInput F5" << (melonds_style_hotkeys_ ? "/melonDS" : "") << ")\n";
        return;
    }
    if (send_retroarch_netcmd("PAUSE_TOGGLE", netcmd_port_)) {
        paused_ = !paused_;
        std::cout
            << "EmulatorControl: pause_toggle → " << (paused_ ? "on" : "off")
            << " (netcmd " << netcmd_port_ << ")\n";
    }
}

void VirtualKeyboard::apply_emulator_control(const EmulatorControl& control) {
    const bool force = control.force != 0;
    if (control.pause == EmulatorControlState::On) {
        set_paused(true, force);
    } else if (control.pause == EmulatorControlState::Off) {
        set_paused(false, force);
    }
    if (control.fast_forward == EmulatorControlState::On) {
        set_fast_forward(true, force);
    } else if (control.fast_forward == EmulatorControlState::Off) {
        set_fast_forward(false, force);
    }
}

void VirtualKeyboard::apply(const KeyboardState& state) {
    if (!plugged_) {
        return;
    }
    const auto previous = has_last_ ? last_keys_ : 0u;
    const auto next = state.keys;
    std::size_t count = 0;
    const auto* bindings = default_remoted_key_bindings(count);
    for (std::size_t i = 0; i < count; ++i) {
        const auto& binding = bindings[i];
        const bool was_down = bit_down(previous, binding.key);
        const bool is_down = bit_down(next, binding.key);
        if (was_down == is_down) {
            continue;
        }
        switch (binding.action) {
        case RemotedKeyAction::NetcmdEdgeToggle:
            if (binding.netcmd != nullptr) {
                send_retroarch_netcmd(binding.netcmd, netcmd_port_);
            }
            break;
        case RemotedKeyAction::NetcmdPress:
            if (!is_down) {
                break;
            }
            if (binding.key == KeyP) {
                // Pause is EmulatorControl-only on every backend (RA netcmd / F5).
                std::cerr
                    << "Keyboard: remoted P ignored — use EmulatorControl pause\n";
                break;
            }
            if (binding.netcmd != nullptr) {
                send_retroarch_netcmd(binding.netcmd, netcmd_port_);
            }
            break;
        case RemotedKeyAction::XTestHold:
        case RemotedKeyAction::Ignored:
            break;
        }
    }
    apply_xtest_edges(previous, next);

    if (switch_style_hotkeys_) {
        const bool space_was = bit_down(previous, KeySpace);
        const bool space_now = bit_down(next, KeySpace);
        if (space_now && !space_was && !fast_forward_) {
            // Desktop remoted Space edge when EmulatorControl does not own FF.
            send_vk(VK_SPACE, true);
            send_vk(VK_SPACE, false);
        }
    } else if (!fast_forward_) {
        set_ff_key_held(bit_down(next, KeySpace));
    }

    last_keys_ = next;
    has_last_ = true;
}

void VirtualKeyboard::release_all() {
    if (ff_key_held_) {
        set_ff_key_held(false);
        fast_forward_ = false;
    }
    if (has_last_ && last_keys_ != 0) {
        apply_xtest_edges(last_keys_, 0);
        last_keys_ = 0;
    }
}

} // namespace archstreamer

#endif // _WIN32
