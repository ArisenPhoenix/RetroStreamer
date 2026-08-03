#include "host/windows_save_profile_paths.hpp"

#include "common/platform/paths.hpp"

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdlib>
#include <string>
#include <system_error>

namespace archstreamer {
namespace {

std::filesystem::path local_app_data() {
    if (const char* local = std::getenv("LOCALAPPDATA"); local != nullptr && *local != '\0') {
        return std::filesystem::path(local);
    }
    const auto home = user_home_directory();
    if (!home.empty()) {
        return std::filesystem::path(home) / "AppData" / "Local";
    }
    return {};
}

std::filesystem::path roaming_app_data() {
    if (const char* appdata = std::getenv("APPDATA"); appdata != nullptr && *appdata != '\0') {
        return std::filesystem::path(appdata);
    }
    const auto home = user_home_directory();
    if (!home.empty()) {
        return std::filesystem::path(home) / "AppData" / "Roaming";
    }
    return {};
}

} // namespace

std::filesystem::path WindowsSaveProfilePaths::default_root() {
    const auto local = local_app_data();
    if (!local.empty()) {
        return local / "archstreamer" / "saves";
    }
    return std::filesystem::current_path() / "archstreamer-saves";
}

std::filesystem::path WindowsSaveProfilePaths::shared_retroarch_system_directory() {
    const auto roaming = roaming_app_data();
    if (!roaming.empty()) {
        return roaming / "retroarch" / "system";
    }
    return std::filesystem::path("retroarch") / "system";
}

void WindowsSaveProfilePaths::link_path(
    const std::filesystem::path& link,
    const std::filesystem::path& target) {
    std::error_code error;
    if (std::filesystem::is_symlink(link, error)) {
        if (std::filesystem::read_symlink(link, error) == target) {
            return;
        }
        std::filesystem::remove(link, error);
    } else if (std::filesystem::exists(link, error)) {
        return;
    }

    // Prefer an unprivileged symlink when Developer Mode allows it; otherwise a
    // directory junction. Regular-file links fall back to copy.
    const auto link_w = link.wstring();
    const auto target_w = target.wstring();
    const bool target_is_dir = std::filesystem::is_directory(target, error);
    DWORD flags = SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE;
    if (target_is_dir) {
        flags |= SYMBOLIC_LINK_FLAG_DIRECTORY;
    }
    if (CreateSymbolicLinkW(link_w.c_str(), target_w.c_str(), flags)) {
        return;
    }
    if (target_is_dir) {
        // Junction via mklink-compatible CreateSymbolicLink without unprivileged flag.
        if (CreateSymbolicLinkW(
                link_w.c_str(),
                target_w.c_str(),
                SYMBOLIC_LINK_FLAG_DIRECTORY)) {
            return;
        }
    }
    std::filesystem::create_symlink(target, link, error);
}

} // namespace archstreamer

#endif // _WIN32
