#include "host/switch/windows_switch_paths.hpp"

#include "host/switch/switch_fs.hpp"

#include "common/platform/paths.hpp"

#include <cstdlib>
#include <iostream>
#include <system_error>

namespace archstreamer {

std::filesystem::path WindowsSwitchPaths::archstreamer_data_root() {
    if (const auto home = switch_home_dir(); !home.empty()) {
        const auto cache = archstreamer_cache_directory();
        if (!cache.empty()) {
            return std::filesystem::path{cache};
        }
        return home / "AppData" / "Local" / "archstreamer";
    }
    return std::filesystem::current_path() / "archstreamer-data";
}

std::filesystem::path WindowsSwitchPaths::yuzu_runtime_root() {
    if (const auto home = switch_home_dir(); !home.empty()) {
        const auto cache = archstreamer_cache_directory();
        if (!cache.empty()) {
            return std::filesystem::path{cache} / "yuzu";
        }
        return home / "AppData" / "Local" / "archstreamer" / "yuzu";
    }
    return std::filesystem::current_path() / "archstreamer-yuzu";
}

std::filesystem::path WindowsSwitchPaths::ryujinx_runtime_root() {
    if (const auto home = switch_home_dir(); !home.empty()) {
        const auto cache = archstreamer_cache_directory();
        if (!cache.empty()) {
            return std::filesystem::path{cache} / "ryujinx";
        }
        return home / "AppData" / "Local" / "archstreamer" / "ryujinx";
    }
    return std::filesystem::current_path() / "archstreamer-ryujinx";
}

std::vector<std::filesystem::path> WindowsSwitchPaths::keys_source_candidates() {
    std::vector<std::filesystem::path> out;
    if (const auto env = switch_path_from_env("ARCHSTREAMER_YUZU_KEYS"); env.has_value()) {
        if (std::filesystem::is_directory(*env)) {
            out.push_back(*env);
        }
    }
    const auto home = switch_home_dir();
    out.push_back(home / "AppData" / "Roaming" / "yuzu" / "keys");
    out.push_back(std::filesystem::path{archstreamer_cache_directory()} / "yuzu" / "keys");
    return out;
}

std::vector<std::filesystem::path> WindowsSwitchPaths::firmware_source_candidates(
    const std::filesystem::path& managed_registered) {
    std::vector<std::filesystem::path> out;
    out.push_back(managed_registered);
    out.push_back(ryujinx_runtime_root() / "firmware" / "registered");
    if (const auto home = switch_home_dir(); !home.empty()) {
        out.push_back(home / ".config" / "Ryujinx" / "bis" / "system" / "Contents" / "registered");
        if (const char* local = std::getenv("LOCALAPPDATA"); local != nullptr && *local != '\0') {
            out.push_back(
                std::filesystem::path{local} / "Ryujinx" / "bis" / "system" / "Contents" / "registered");
        }
    }
    return out;
}

std::vector<std::filesystem::path> WindowsSwitchPaths::profiles_template_source_candidates() {
    const auto home = switch_home_dir();
    return {
        home / ".config" / "Ryujinx" / "system" / "Profiles.json",
        home / "AppData" / "Local" / "Ryujinx" / "system" / "Profiles.json",
    };
}

void WindowsSwitchPaths::bind_ryujinx_firmware(
    const std::filesystem::path& managed_registered,
    const std::filesystem::path& profile_registered) {
    std::error_code ec;
    if (std::filesystem::exists(profile_registered, ec)) {
        std::filesystem::remove_all(profile_registered, ec);
    }
    std::filesystem::create_directories(profile_registered.parent_path());
    switch_copy_directory_recursive_skip_existing(managed_registered, profile_registered);
    std::cout << "Ryujinx firmware: seeded " << profile_registered << '\n';
}

} // namespace archstreamer
