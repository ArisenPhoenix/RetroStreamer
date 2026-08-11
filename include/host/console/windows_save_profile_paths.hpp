#pragma once

#include <filesystem>

namespace archstreamer {

/**
 * Windows save / RetroArch-system roots (%LOCALAPPDATA%, %APPDATA%).
 * Public call sites still use default_save_profile_root() /
 * shared_retroarch_system_directory(); those delegate here on Win32.
 * Linux keeps the reference implementations in save_profile.cpp until a
 * matching PosixSaveProfilePaths wraps them.
 */
class WindowsSaveProfilePaths {
public:
    static std::filesystem::path default_root();
    static std::filesystem::path shared_retroarch_system_directory();

    /** Directory symlink/junction; falls back to no-op leave-existing on failure. */
    static void link_path(
        const std::filesystem::path& link,
        const std::filesystem::path& target);
};

} // namespace archstreamer
