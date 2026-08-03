#include "host/virtual_keyboard.hpp"

#include "host/soft_keyboard_host.hpp"

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>

#include <chrono>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace archstreamer {
namespace {

bool is_soft_keyboard_dialog_title(const std::string& title) {
    return title.find("Software Keyboard") != std::string::npos ||
        title.find("ContentDialogOverlayWindow") != std::string::npos;
}

std::string window_title_utf8(HWND hwnd) {
    wchar_t title_w[512]{};
    const int n = GetWindowTextW(hwnd, title_w, 512);
    if (n <= 0) {
        return {};
    }
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, title_w, n, nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) {
        return {};
    }
    std::string out(static_cast<std::size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, title_w, n, out.data(), bytes, nullptr, nullptr);
    return out;
}

bool window_belongs_to_pid_tree(HWND hwnd, DWORD owner_pid) {
    if (owner_pid == 0) {
        return true;
    }
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0) {
        return false;
    }
    if (pid == owner_pid) {
        return true;
    }
    // Walk parents a few levels via Toolhelp is expensive per window; allow any
    // child process by checking process parent chain once.
    for (int depth = 0; depth < 8; ++depth) {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE) {
            return false;
        }
        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        DWORD parent = 0;
        bool found = false;
        if (Process32FirstW(snap, &entry)) {
            do {
                if (entry.th32ProcessID == pid) {
                    parent = entry.th32ParentProcessID;
                    found = true;
                    break;
                }
            } while (Process32NextW(snap, &entry));
        }
        CloseHandle(snap);
        if (!found || parent == 0) {
            return false;
        }
        if (parent == owner_pid) {
            return true;
        }
        pid = parent;
    }
    return false;
}

struct FindDialogCtx {
    DWORD owner_pid = 0;
    HWND found = nullptr;
};

BOOL CALLBACK enum_soft_keyboard_proc(HWND hwnd, LPARAM lparam) {
    auto* ctx = reinterpret_cast<FindDialogCtx*>(lparam);
    if (!IsWindowVisible(hwnd)) {
        return TRUE;
    }
    if (!window_belongs_to_pid_tree(hwnd, ctx->owner_pid)) {
        return TRUE;
    }
    const auto title = window_title_utf8(hwnd);
    if (is_soft_keyboard_dialog_title(title)) {
        ctx->found = hwnd;
        return FALSE;
    }
    return TRUE;
}

HWND find_soft_keyboard_dialog(DWORD owner_pid) {
    FindDialogCtx ctx{owner_pid, nullptr};
    EnumWindows(enum_soft_keyboard_proc, reinterpret_cast<LPARAM>(&ctx));
    return ctx.found;
}

HWND find_focused_window() {
    GUITHREADINFO info{};
    info.cbSize = sizeof(info);
    if (GetGUIThreadInfo(0, &info) && info.hwndFocus != nullptr) {
        return info.hwndFocus;
    }
    return GetForegroundWindow();
}

bool focus_window(HWND hwnd) {
    if (hwnd == nullptr) {
        return false;
    }
    // Allow SetForegroundWindow from a background host thread.
    const HWND foreground = GetForegroundWindow();
    const DWORD fore_tid = foreground != nullptr ? GetWindowThreadProcessId(foreground, nullptr) : 0;
    const DWORD this_tid = GetCurrentThreadId();
    if (fore_tid != 0 && fore_tid != this_tid) {
        AttachThreadInput(this_tid, fore_tid, TRUE);
    }
    ShowWindow(hwnd, SW_RESTORE);
    BringWindowToTop(hwnd);
    SetForegroundWindow(hwnd);
    SetFocus(hwnd);
    if (fore_tid != 0 && fore_tid != this_tid) {
        AttachThreadInput(this_tid, fore_tid, FALSE);
    }
    return GetForegroundWindow() == hwnd || GetFocus() == hwnd;
}

void send_vk(WORD vk, bool down) {
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = vk;
    input.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
    SendInput(1, &input, sizeof(INPUT));
}

void send_unicode_char(wchar_t ch) {
    INPUT down{};
    down.type = INPUT_KEYBOARD;
    down.ki.wScan = ch;
    down.ki.dwFlags = KEYEVENTF_UNICODE;
    INPUT up = down;
    up.ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
    SendInput(1, &down, sizeof(INPUT));
    SendInput(1, &up, sizeof(INPUT));
}

void type_utf8_text(const std::string& text) {
    if (text.empty()) {
        return;
    }
    const int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (n <= 0) {
        return;
    }
    std::wstring wide(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), n);
    for (wchar_t ch : wide) {
        if (ch == L'\n' || ch == L'\r') {
            continue;
        }
        send_unicode_char(ch);
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
    }
}

bool try_autofill(const std::string& text, DWORD owner_pid, bool allow_any_focused) {
    HWND target = find_soft_keyboard_dialog(owner_pid);
    if (target == nullptr && allow_any_focused) {
        HWND focus = find_focused_window();
        if (focus != nullptr && IsWindowVisible(focus) &&
            window_belongs_to_pid_tree(focus, owner_pid)) {
            target = focus;
        }
    }
    if (target == nullptr) {
        return false;
    }
    focus_window(target);
    std::this_thread::sleep_for(std::chrono::milliseconds(250));

    // Clear Ryujinx's seeded nickname before typing.
    send_vk(VK_CONTROL, true);
    send_vk('A', true);
    send_vk('A', false);
    send_vk(VK_CONTROL, false);
    send_vk(VK_BACK, true);
    send_vk(VK_BACK, false);
    std::this_thread::sleep_for(std::chrono::milliseconds(80));

    type_utf8_text(text);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    send_vk(VK_RETURN, true);
    send_vk(VK_RETURN, false);
    std::cout << "Ryujinx Software Keyboard: injected \"" << text << "\" (SendInput)\n";
    return true;
}

enum class TextDialogProbe {
    NoDialog,
    Ready,
};

TextDialogProbe probe_text_dialog(DWORD owner_pid) {
    HWND dialog = find_soft_keyboard_dialog(owner_pid);
    if (dialog == nullptr) {
        return TextDialogProbe::NoDialog;
    }
    // Prefer dialog that currently holds focus (or is an ancestor of focus).
    HWND focus = find_focused_window();
    if (focus == nullptr) {
        return TextDialogProbe::Ready;
    }
    HWND current = focus;
    for (int depth = 0; depth < 64 && current != nullptr; ++depth) {
        if (current == dialog) {
            return TextDialogProbe::Ready;
        }
        current = GetParent(current);
    }
    // Dialog exists but may not be focused yet — still treat as Ready so we prompt.
    return TextDialogProbe::Ready;
}

} // namespace

void schedule_soft_keyboard(
    std::shared_ptr<SoftKeyboardHostBridge> bridge,
    std::string fallback_text,
    std::string prompt,
    std::string preferred_display,
    int owner_pid) {
    (void)fallback_text;
    (void)preferred_display; // No nested X display on Windows; scope by owner_pid.
    if (prompt.empty()) {
        prompt = "The game is asking for text. Enter it with the pad.";
    }
    if (!bridge) {
        bridge = std::make_shared<SoftKeyboardHostBridge>();
    }

    std::weak_ptr<SoftKeyboardHostBridge> weak_bridge = bridge;
    const DWORD pid = owner_pid > 0 ? static_cast<DWORD>(owner_pid) : 0;

    std::thread([weak_bridge = std::move(weak_bridge),
                 prompt = std::move(prompt),
                 pid]() {
        constexpr auto kFastInterval = std::chrono::milliseconds(150);
        constexpr auto kIdleInterval = std::chrono::milliseconds(500);
        constexpr int kFastAttempts = 400;

        enum class ServeOutcome {
            Abort,
            Injected,
            NeedsReprompt,
            DialogGone,
        };

        const auto try_manual_inject = [&](const std::string& text) {
            std::string trimmed = text;
            if (trimmed.size() > 12) {
                trimmed.resize(12);
            }
            std::cout
                << "Ryujinx Software Keyboard: manual pad OSK text \"" << trimmed
                << "\" — looking for a dialog to fill\n";
            for (int attempt = 0; attempt < 8; ++attempt) {
                if (try_autofill(trimmed, pid, /*allow_any_focused=*/true)) {
                    return true;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(150));
            }
            std::cerr
                << "Ryujinx Software Keyboard: manual inject failed — no usable dialog found\n";
            return false;
        };

        const auto drain_manual_inject = [&]() {
            auto locked = weak_bridge.lock();
            if (!locked) {
                return false;
            }
            auto text = locked->take_manual_inject();
            locked.reset();
            if (!text.has_value()) {
                return false;
            }
            try_manual_inject(*text);
            return true;
        };

        const auto wait_for_dialog = [&]() {
            for (int attempt = 0;; ++attempt) {
                if (weak_bridge.expired()) {
                    return false;
                }
                if (drain_manual_inject()) {
                    continue;
                }
                if (probe_text_dialog(pid) == TextDialogProbe::Ready) {
                    return true;
                }
                std::this_thread::sleep_for(
                    attempt < kFastAttempts ? kFastInterval : kIdleInterval);
            }
        };

        const auto serve_dialog = [&]() -> ServeOutcome {
            SoftKeyboardRequest request;
            {
                auto locked = weak_bridge.lock();
                if (!locked) {
                    return ServeOutcome::Abort;
                }
                {
                    std::lock_guard lock(locked->mutex);
                    request = locked->make_request(prompt, /*initial_text=*/{}, 12);
                }
                locked->publish_request(request);
            }
            std::cout
                << "Ryujinx Software Keyboard: focused text dialog"
                << " — requesting pad OSK (id=" << request.request_id << ")\n";

            std::optional<SoftKeyboardResponse> response;
            std::optional<std::string> manual_text;
            for (int wait = 0; wait < 360; ++wait) {
                auto locked = weak_bridge.lock();
                if (!locked) {
                    return ServeOutcome::Abort;
                }
                response = locked->take_response();
                if (response.has_value() && response->request_id == request.request_id) {
                    break;
                }
                response.reset();
                manual_text = locked->take_manual_inject();
                if (manual_text.has_value()) {
                    break;
                }
                locked.reset();
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }

            std::string text;
            const bool from_manual = manual_text.has_value() && !manual_text->empty();
            const bool from_accept = response.has_value() && response->accepted &&
                !response->text.empty();
            if (from_manual) {
                text = *manual_text;
            } else if (from_accept) {
                text = response->text;
            } else {
                if (probe_text_dialog(pid) == TextDialogProbe::Ready) {
                    return ServeOutcome::NeedsReprompt;
                }
                return ServeOutcome::DialogGone;
            }
            if (text.size() > 12) {
                text.resize(12);
            }
            for (int attempt = 0; attempt < 10; ++attempt) {
                if (try_autofill(text, pid, /*allow_any_focused=*/false)) {
                    return ServeOutcome::Injected;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(150));
            }
            // Last resort: type into whatever is focused under the owner tree.
            if (try_autofill(text, pid, /*allow_any_focused=*/true)) {
                return ServeOutcome::Injected;
            }
            std::cerr << "Ryujinx Software Keyboard: inject failed after pad OSK accept\n";
            return ServeOutcome::NeedsReprompt;
        };

        while (true) {
            if (!wait_for_dialog()) {
                return;
            }
            const auto outcome = serve_dialog();
            if (outcome == ServeOutcome::Abort) {
                return;
            }
            if (outcome == ServeOutcome::Injected) {
                // Wait for the dialog to dismiss before hunting for the next prompt.
                for (int i = 0; i < 100; ++i) {
                    if (weak_bridge.expired()) {
                        return;
                    }
                    if (probe_text_dialog(pid) == TextDialogProbe::NoDialog) {
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                continue;
            }
            if (outcome == ServeOutcome::NeedsReprompt) {
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                continue;
            }
            // DialogGone — resume idle wait.
        }
    }).detach();
}

void ensure_soft_keyboard(
    std::shared_ptr<SoftKeyboardHostBridge>& bridge,
    std::string fallback_text,
    std::string prompt,
    std::string preferred_display,
    int owner_pid) {
    if (!bridge) {
        bridge = std::make_shared<SoftKeyboardHostBridge>();
    }
    schedule_soft_keyboard(
        bridge,
        std::move(fallback_text),
        std::move(prompt),
        std::move(preferred_display),
        owner_pid);
}

std::vector<std::string> xtest_display_candidates(const std::string&) {
    return {};
}

std::vector<std::string> soft_keyboard_display_candidates(const std::string&, int) {
    return {};
}

} // namespace archstreamer

#endif // _WIN32
