#include "client/video_window_geometry.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <string>

namespace archstreamer {
namespace {

bool title_looks_like_gst_video(const std::wstring& title) {
    static const wchar_t* kNeedles[] = {
        L"gst-launch", L"Direct3D11", L"D3D11", L"GStreamer", L"videosink",
    };
    for (const auto* needle : kNeedles) {
        if (title.find(needle) != std::wstring::npos) {
            return true;
        }
    }
    return false;
}

struct FindVideoWindow {
    DWORD pid = 0;
    HWND best = nullptr;
    int best_area = 0;
};

BOOL CALLBACK enum_video_window(HWND window, LPARAM param) {
    auto* search = reinterpret_cast<FindVideoWindow*>(param);
    if (!IsWindowVisible(window)) {
        return TRUE;
    }
    if (search->pid != 0) {
        DWORD owner = 0;
        GetWindowThreadProcessId(window, &owner);
        if (owner != search->pid) {
            return TRUE;
        }
    } else {
        wchar_t title[512] = {};
        GetWindowTextW(window, title, static_cast<int>(std::size(title)));
        if (!title_looks_like_gst_video(title)) {
            return TRUE;
        }
    }
    RECT rect{};
    if (!GetWindowRect(window, &rect)) {
        return TRUE;
    }
    const int area = (rect.right - rect.left) * (rect.bottom - rect.top);
    if (area > search->best_area) {
        search->best_area = area;
        search->best = window;
    }
    return TRUE;
}

} // namespace

bool raise_video_window(int pid) {
    FindVideoWindow search;
    search.pid = pid > 0 ? static_cast<DWORD>(pid) : 0;
    EnumWindows(&enum_video_window, reinterpret_cast<LPARAM>(&search));
    if (search.best == nullptr) {
        return false;
    }

    // Windows only grants SetForegroundWindow to the thread that owns the current
    // foreground window, so borrow its input state for the duration of the call.
    const HWND foreground = GetForegroundWindow();
    const DWORD our_thread = GetCurrentThreadId();
    const DWORD foreground_thread =
        foreground != nullptr ? GetWindowThreadProcessId(foreground, nullptr) : 0;
    const bool attached = foreground_thread != 0 && foreground_thread != our_thread &&
        AttachThreadInput(our_thread, foreground_thread, TRUE) != 0;

    BringWindowToTop(search.best);
    SetForegroundWindow(search.best);

    if (attached) {
        AttachThreadInput(our_thread, foreground_thread, FALSE);
    }
    return true;
}

// d3d11videosink window placement across gst-launch restarts is not wired yet.
VideoWindowGeometry capture_video_window_geometry(int) {
    return {};
}

bool apply_video_window_geometry(
    int,
    const VideoWindowGeometry&,
    std::chrono::milliseconds) {
    return false;
}

int primary_display_height() {
    const int height = GetSystemMetrics(SM_CYSCREEN);
    return height > 0 ? height : 0;
}

} // namespace archstreamer
