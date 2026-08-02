#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <functional>
#include <mutex>
#include <vector>

namespace archstreamer {

/**
 * GUI ↔ session bridge for Qt-embedded video.
 * Frames are RGBA from in-process appsink (no X11 PutImage — avoids compositor stalls).
 */
struct VideoEmbedBridge {
    std::mutex mutex;
    int width = 0;
    int height = 0;
    std::uint64_t serial = 0;
    std::atomic_bool expose_requested{false};

    std::mutex frame_mutex;
    std::vector<std::uint8_t> frame_rgba;
    int frame_w = 0;
    int frame_h = 0;
    int frame_stride = 0;
    std::uint64_t frame_serial = 0;

    std::mutex stop_mutex;
    std::function<void()> emergency_stop;

    void set_size(int w, int h) {
        if (w <= 0 || h <= 0) {
            return;
        }
        std::lock_guard lock(mutex);
        if (w == width && h == height) {
            return;
        }
        width = w;
        height = h;
        ++serial;
    }

    void request_expose() {
        expose_requested.store(true, std::memory_order_release);
    }

    bool take_expose() {
        return expose_requested.exchange(false, std::memory_order_acq_rel);
    }

    /** Returns true when size changed since last_serial; updates last_serial. */
    bool take_size(int& w, int& h, std::uint64_t& last_serial) {
        std::lock_guard lock(mutex);
        if (serial == last_serial) {
            return false;
        }
        last_serial = serial;
        w = width;
        h = height;
        return w > 0 && h > 0;
    }

    void publish_frame(const std::uint8_t* data, int w, int h, int stride) {
        if (data == nullptr || w <= 0 || h <= 0 || stride < w * 4) {
            return;
        }
        std::lock_guard lock(frame_mutex);
        frame_w = w;
        frame_h = h;
        frame_stride = stride;
        frame_rgba.resize(static_cast<std::size_t>(stride) * static_cast<std::size_t>(h));
        std::memcpy(frame_rgba.data(), data, frame_rgba.size());
        ++frame_serial;
    }

    /** Copy latest frame if newer than last_serial. */
    bool copy_frame(
        std::vector<std::uint8_t>& out,
        int& w,
        int& h,
        int& stride,
        std::uint64_t& last_serial) {
        std::lock_guard lock(frame_mutex);
        if (frame_serial == last_serial || frame_w <= 0 || frame_h <= 0) {
            return false;
        }
        last_serial = frame_serial;
        w = frame_w;
        h = frame_h;
        stride = frame_stride;
        out = frame_rgba;
        return true;
    }

    void set_emergency_stop(std::function<void()> fn) {
        std::lock_guard lock(stop_mutex);
        emergency_stop = std::move(fn);
    }

    void clear_emergency_stop() {
        std::lock_guard lock(stop_mutex);
        emergency_stop = nullptr;
    }

    /** Stop the in-process pipeline immediately (safe from the GUI thread). */
    void run_emergency_stop() {
        std::function<void()> fn;
        {
            std::lock_guard lock(stop_mutex);
            fn = emergency_stop;
        }
        if (fn) {
            fn();
        }
    }
};

} // namespace archstreamer
