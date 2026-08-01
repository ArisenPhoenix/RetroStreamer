#include "host/session_slot_lease.hpp"

#include "common/platform/paths.hpp"

#include <cstdlib>
#include <filesystem>
#include <system_error>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

namespace archstreamer::slot_lock {

std::string lock_directory() {
    std::filesystem::path root;
    if (const char* runtime = std::getenv("XDG_RUNTIME_DIR"); runtime != nullptr && *runtime != '\0') {
        root = std::filesystem::path{runtime} / "archstreamer";
    } else {
        root = std::filesystem::path{archstreamer_cache_directory()} / "slots";
    }
    std::error_code error;
    std::filesystem::create_directories(root, error);
    return root.string();
}

std::intptr_t try_acquire(const std::string& path) {
    const int descriptor = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (descriptor < 0) {
        return -1;
    }
    // flock is released by the kernel if the host crashes, so a stale lock file
    // never blocks the next run.
    if (::flock(descriptor, LOCK_EX | LOCK_NB) != 0) {
        ::close(descriptor);
        return -1;
    }
    return static_cast<std::intptr_t>(descriptor);
}

void release(std::intptr_t handle) {
    const auto descriptor = static_cast<int>(handle);
    if (descriptor < 0) {
        return;
    }
    ::flock(descriptor, LOCK_UN);
    ::close(descriptor);
}

bool display_number_free(int display_number) {
    if (display_number < 0) {
        return true;
    }
    const auto suffix = std::to_string(display_number);
    std::error_code error;
    if (std::filesystem::exists("/tmp/.X11-unix/X" + suffix, error)) {
        return false;
    }
    return !std::filesystem::exists("/tmp/.X" + suffix + "-lock", error);
}

} // namespace archstreamer::slot_lock
