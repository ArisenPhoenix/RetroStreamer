#include "host/switch/posix_switch_paths.hpp"

#include "host/switch/switch_fs.hpp"

#include "common/platform/paths.hpp"

#include <cstdlib>
#include <iostream>
#include <system_error>

namespace archstreamer {

std::filesystem::path PosixSwitchPaths::archstreamer_data_root() {
    if (const auto home = switch_home_dir(); !home.empty()) {
        return home / ".local/share/archstreamer";
    }
    return std::filesystem::current_path() / "archstreamer-data";
}

std::filesystem::path PosixSwitchPaths::yuzu_runtime_root() {
    if (const auto home = switch_home_dir(); !home.empty()) {
        return home / ".local/share/archstreamer/yuzu";
    }
    return std::filesystem::current_path() / "archstreamer-yuzu";
}

std::filesystem::path PosixSwitchPaths::ryujinx_runtime_root() {
    if (const auto home = switch_home_dir(); !home.empty()) {
        return home / ".local/share/archstreamer/ryujinx";
    }
    return std::filesystem::current_path() / "archstreamer-ryujinx";
}

std::vector<std::filesystem::path> PosixSwitchPaths::keys_source_candidates() {
    std::vector<std::filesystem::path> out;
    if (const auto env = switch_path_from_env("ARCHSTREAMER_YUZU_KEYS"); env.has_value()) {
        if (std::filesystem::is_directory(*env)) {
            out.push_back(*env);
        }
    }
    const auto home = switch_home_dir();
    out.push_back(home / ".local/share/yuzu/keys");
    out.push_back(home / ".config/yuzu/keys");
    out.push_back("/srv/retroarch/system/yuzu/keys");
    out.push_back("/srv/retroarch/system/keys");
    return out;
}

std::vector<std::filesystem::path> PosixSwitchPaths::firmware_source_candidates(
    const std::filesystem::path& managed_registered) {
    std::vector<std::filesystem::path> out;
    out.push_back(managed_registered);
    out.push_back(ryujinx_runtime_root() / "firmware" / "registered");
    if (const auto home = switch_home_dir(); !home.empty()) {
        out.push_back(home / ".config" / "Ryujinx" / "bis" / "system" / "Contents" / "registered");
    }
    return out;
}

std::vector<std::filesystem::path> PosixSwitchPaths::profiles_template_source_candidates() {
    const auto home = switch_home_dir();
    return {home / ".config" / "Ryujinx" / "system" / "Profiles.json"};
}

void PosixSwitchPaths::bind_ryujinx_firmware(
    const std::filesystem::path& managed_registered,
    const std::filesystem::path& profile_registered) {
    std::error_code ec;
    if (std::filesystem::exists(profile_registered, ec)) {
        std::filesystem::remove_all(profile_registered, ec);
    }
    std::filesystem::create_directories(profile_registered.parent_path());

    std::filesystem::create_directory_symlink(managed_registered, profile_registered, ec);
    if (!ec && switch_directory_has_entries(profile_registered)) {
        std::cout << "Ryujinx firmware: linked " << profile_registered << " -> " << managed_registered
                  << '\n';
        return;
    }
    ec.clear();
    if (std::filesystem::exists(profile_registered)) {
        std::filesystem::remove_all(profile_registered, ec);
    }
    switch_copy_directory_recursive_skip_existing(managed_registered, profile_registered);
    std::cout << "Ryujinx firmware: seeded " << profile_registered << '\n';
}

} // namespace archstreamer
