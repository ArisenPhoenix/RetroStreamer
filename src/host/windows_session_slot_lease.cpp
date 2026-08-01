#include "host/session_slot_lease.hpp"

#include "common/platform/paths.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <filesystem>
#include <system_error>

namespace archstreamer::slot_lock {

std::string lock_directory() {
    const auto root = std::filesystem::path{archstreamer_cache_directory()} / "slots";
    std::error_code error;
    std::filesystem::create_directories(root, error);
    return root.string();
}

std::intptr_t try_acquire(const std::string& path) {
    // Zero share mode: the handle is the lock, and Windows drops it when the
    // process dies, so a crashed host never strands a slot.
    const HANDLE handle = ::CreateFileA(
        path.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        0,
        nullptr,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return -1;
    }
    return reinterpret_cast<std::intptr_t>(handle);
}

void release(std::intptr_t handle) {
    if (handle == -1) {
        return;
    }
    ::CloseHandle(reinterpret_cast<HANDLE>(handle));
}

// Windows capture duplicates the desktop; there is no X display to share.
bool display_number_free(int) {
    return true;
}

} // namespace archstreamer::slot_lock
