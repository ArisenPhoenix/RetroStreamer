#include "client/video_window_geometry.hpp"

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <algorithm>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

namespace archstreamer {
namespace {

Display* open_display() {
    return XOpenDisplay(nullptr);
}

bool read_cardinal_pid(Display* display, Window window, Atom pid_atom, int* out_pid) {
    Atom actual_type = None;
    int actual_format = 0;
    unsigned long item_count = 0;
    unsigned long bytes_after = 0;
    unsigned char* prop = nullptr;
    if (XGetWindowProperty(
            display,
            window,
            pid_atom,
            0,
            1,
            False,
            XA_CARDINAL,
            &actual_type,
            &actual_format,
            &item_count,
            &bytes_after,
            &prop) != Success ||
        prop == nullptr ||
        item_count < 1 ||
        actual_format != 32) {
        if (prop != nullptr) {
            XFree(prop);
        }
        return false;
    }
    *out_pid = static_cast<int>(*reinterpret_cast<long*>(prop));
    XFree(prop);
    return true;
}

std::vector<Window> client_list(Display* display) {
    std::vector<Window> windows;
    const Atom list_atom = XInternAtom(display, "_NET_CLIENT_LIST", False);
    Atom actual_type = None;
    int actual_format = 0;
    unsigned long item_count = 0;
    unsigned long bytes_after = 0;
    unsigned char* prop = nullptr;
    if (XGetWindowProperty(
            display,
            DefaultRootWindow(display),
            list_atom,
            0,
            16384,
            False,
            XA_WINDOW,
            &actual_type,
            &actual_format,
            &item_count,
            &bytes_after,
            &prop) != Success ||
        prop == nullptr) {
        return windows;
    }
    const auto* ids = reinterpret_cast<Window*>(prop);
    windows.assign(ids, ids + item_count);
    XFree(prop);
    return windows;
}

bool window_mapped(Display* display, Window window) {
    XWindowAttributes attrs{};
    if (!XGetWindowAttributes(display, window, &attrs)) {
        return false;
    }
    return attrs.map_state == IsViewable && attrs.width >= 32 && attrs.height >= 32;
}

Window find_window_for_pid(Display* display, int pid) {
    if (pid <= 0) {
        return None;
    }
    const Atom pid_atom = XInternAtom(display, "_NET_WM_PID", False);
    Window best = None;
    int best_area = 0;
    for (const Window window : client_list(display)) {
        int owner = 0;
        if (!read_cardinal_pid(display, window, pid_atom, &owner) || owner != pid) {
            continue;
        }
        if (!window_mapped(display, window)) {
            continue;
        }
        XWindowAttributes attrs{};
        if (!XGetWindowAttributes(display, window, &attrs)) {
            continue;
        }
        const int area = attrs.width * attrs.height;
        if (area > best_area) {
            best_area = area;
            best = window;
        }
    }
    return best;
}

std::string window_title(Display* display, Window window) {
    Atom actual_type = None;
    int actual_format = 0;
    unsigned long item_count = 0;
    unsigned long bytes_after = 0;
    unsigned char* prop = nullptr;
    const Atom name_atom = XInternAtom(display, "_NET_WM_NAME", False);
    if (XGetWindowProperty(
            display,
            window,
            name_atom,
            0,
            256,
            False,
            AnyPropertyType,
            &actual_type,
            &actual_format,
            &item_count,
            &bytes_after,
            &prop) == Success &&
        prop != nullptr) {
        std::string name(reinterpret_cast<char*>(prop));
        XFree(prop);
        return name;
    }
    char* legacy = nullptr;
    if (XFetchName(display, window, &legacy) && legacy != nullptr) {
        std::string name(legacy);
        XFree(legacy);
        return name;
    }
    return {};
}

bool title_looks_like_gst_video(const std::string& title) {
    return title.find("gst-launch") != std::string::npos ||
        title.find("ximagesink") != std::string::npos ||
        title.find("xvimagesink") != std::string::npos ||
        title.find("glimagesink") != std::string::npos ||
        title.find("GStreamer") != std::string::npos;
}

Window find_gst_video_window(Display* display, int pid) {
    if (const Window by_pid = find_window_for_pid(display, pid); by_pid != None) {
        return by_pid;
    }
    // Some sinks omit _NET_WM_PID; fall back to the largest mapped gst window.
    Window best = None;
    int best_area = 0;
    for (const Window window : client_list(display)) {
        if (!window_mapped(display, window) || !title_looks_like_gst_video(window_title(display, window))) {
            continue;
        }
        XWindowAttributes attrs{};
        if (!XGetWindowAttributes(display, window, &attrs)) {
            continue;
        }
        const int area = attrs.width * attrs.height;
        if (area > best_area) {
            best_area = area;
            best = window;
        }
    }
    return best;
}

void read_wm_state(Display* display, Window window, bool* maximized, bool* fullscreen) {
    *maximized = false;
    *fullscreen = false;
    const Atom state_atom = XInternAtom(display, "_NET_WM_STATE", False);
    const Atom max_horz = XInternAtom(display, "_NET_WM_STATE_MAXIMIZED_HORZ", False);
    const Atom max_vert = XInternAtom(display, "_NET_WM_STATE_MAXIMIZED_VERT", False);
    const Atom full = XInternAtom(display, "_NET_WM_STATE_FULLSCREEN", False);
    Atom actual_type = None;
    int actual_format = 0;
    unsigned long item_count = 0;
    unsigned long bytes_after = 0;
    unsigned char* prop = nullptr;
    if (XGetWindowProperty(
            display,
            window,
            state_atom,
            0,
            64,
            False,
            XA_ATOM,
            &actual_type,
            &actual_format,
            &item_count,
            &bytes_after,
            &prop) != Success ||
        prop == nullptr) {
        return;
    }
    const auto* atoms = reinterpret_cast<Atom*>(prop);
    bool horz = false;
    bool vert = false;
    for (unsigned long i = 0; i < item_count; ++i) {
        if (atoms[i] == max_horz) {
            horz = true;
        } else if (atoms[i] == max_vert) {
            vert = true;
        } else if (atoms[i] == full) {
            *fullscreen = true;
        }
    }
    *maximized = horz && vert;
    XFree(prop);
}

bool read_geometry(Display* display, Window window, VideoWindowGeometry* out) {
    XWindowAttributes attrs{};
    if (!XGetWindowAttributes(display, window, &attrs)) {
        return false;
    }
    int root_x = 0;
    int root_y = 0;
    Window child = None;
    if (!XTranslateCoordinates(
            display, window, attrs.root, 0, 0, &root_x, &root_y, &child)) {
        return false;
    }
    out->valid = true;
    out->x = root_x;
    out->y = root_y;
    out->width = attrs.width;
    out->height = attrs.height;
    read_wm_state(display, window, &out->maximized, &out->fullscreen);
    return true;
}

void send_net_wm_state(
    Display* display,
    Window window,
    long action,
    Atom state1,
    Atom state2 = None) {
    XEvent event{};
    event.xclient.type = ClientMessage;
    event.xclient.window = window;
    event.xclient.message_type = XInternAtom(display, "_NET_WM_STATE", False);
    event.xclient.format = 32;
    event.xclient.data.l[0] = action;
    event.xclient.data.l[1] = static_cast<long>(state1);
    event.xclient.data.l[2] = static_cast<long>(state2);
    event.xclient.data.l[3] = 1; // application
    XSendEvent(
        display,
        DefaultRootWindow(display),
        False,
        SubstructureRedirectMask | SubstructureNotifyMask,
        &event);
}

constexpr long kNetWmStateAdd = 1;
constexpr long kNetWmStateRemove = 0;

void apply_to_window(Display* display, Window window, const VideoWindowGeometry& geometry) {
    // Clear expand/fullscreen first so a normal restore is not fighting the WM.
    const Atom max_horz = XInternAtom(display, "_NET_WM_STATE_MAXIMIZED_HORZ", False);
    const Atom max_vert = XInternAtom(display, "_NET_WM_STATE_MAXIMIZED_VERT", False);
    const Atom full = XInternAtom(display, "_NET_WM_STATE_FULLSCREEN", False);
    send_net_wm_state(display, window, kNetWmStateRemove, max_horz, max_vert);
    send_net_wm_state(display, window, kNetWmStateRemove, full);

    XMoveResizeWindow(
        display,
        window,
        geometry.x,
        geometry.y,
        static_cast<unsigned int>(std::max(32, geometry.width)),
        static_cast<unsigned int>(std::max(32, geometry.height)));

    if (geometry.fullscreen) {
        send_net_wm_state(display, window, kNetWmStateAdd, full);
    } else if (geometry.maximized) {
        send_net_wm_state(display, window, kNetWmStateAdd, max_horz, max_vert);
    }
    XFlush(display);
}

} // namespace

VideoWindowGeometry capture_video_window_geometry(int pid) {
    VideoWindowGeometry geometry{};
    Display* display = open_display();
    if (display == nullptr) {
        return geometry;
    }
    const Window window = find_gst_video_window(display, pid);
    if (window != None) {
        read_geometry(display, window, &geometry);
    }
    XCloseDisplay(display);
    return geometry;
}

bool apply_video_window_geometry(
    int pid,
    const VideoWindowGeometry& geometry,
    std::chrono::milliseconds timeout) {
    if (!geometry.valid || pid <= 0) {
        return false;
    }
    Display* display = open_display();
    if (display == nullptr) {
        return false;
    }

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    Window window = None;
    while (std::chrono::steady_clock::now() < deadline) {
        window = find_gst_video_window(display, pid);
        if (window != None) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (window == None) {
        XCloseDisplay(display);
        return false;
    }

    // ximagesink maps before the first frame; give the WM a beat to decorate it.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    apply_to_window(display, window, geometry);
    XCloseDisplay(display);
    return true;
}

int primary_display_height() {
    Display* display = open_display();
    if (display == nullptr) {
        return 0;
    }
    const int height = DisplayHeight(display, DefaultScreen(display));
    XCloseDisplay(display);
    return height > 0 ? height : 0;
}

} // namespace archstreamer
