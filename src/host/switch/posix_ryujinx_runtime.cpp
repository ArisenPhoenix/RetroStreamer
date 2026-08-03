#include "host/switch/posix_ryujinx_runtime.hpp"

#include "host/switch/default_switch_paths.hpp"
#include "host/switch/switch_fs.hpp"
#include "host/switch/switch_system_defaults.hpp"

#include <iostream>
#include <string>
#include <vector>

namespace archstreamer {

std::filesystem::path PosixRyujinxRuntime::runtime_root() {
    return SwitchPaths::ryujinx_runtime_root();
}

std::optional<std::filesystem::path> PosixRyujinxRuntime::find_source_binary() {
    if (const auto env = switch_path_from_env("ARCHSTREAMER_RYUJINX"); env.has_value()) {
        if (std::filesystem::is_regular_file(*env)) {
            return env;
        }
        if (std::filesystem::is_directory(*env)) {
            for (const char* name : {"Ryujinx", "ryujinx", "Ryujinx.AppImage", "ryujinx.AppImage"}) {
                const auto exe = *env / name;
                if (std::filesystem::is_regular_file(exe)) {
                    return exe;
                }
            }
        }
    }

    const auto home = switch_home_dir();
    const std::vector<std::filesystem::path> candidates{
        "/srv/emus/Ryujinx.AppImage",
        "/srv/emus/Ryujinx",
        "/srv/emus/ryujinx",
        // Legacy layout (pre-standalone consolidation).
        "/srv/emus/Switch/Ryujinx.AppImage",
        "/srv/emus/Switch/Ryujinx",
        "/srv/emus/Switch/ryujinx",
        home / "Applications/Ryujinx.AppImage",
        home / "Applications/Ryujinx",
        home / ".local/bin/Ryujinx",
        home / ".local/bin/ryujinx",
        "/usr/local/bin/Ryujinx",
        "/usr/local/bin/ryujinx",
        "/usr/bin/Ryujinx",
        "/usr/bin/ryujinx",
    };
    for (const auto& candidate : candidates) {
        if (std::filesystem::is_regular_file(candidate)) {
            return candidate;
        }
    }
    return std::nullopt;
}

bool PosixRyujinxRuntime::available() {
    const auto managed_binary = runtime_root() / "Ryujinx";
    const auto managed_appimage = runtime_root() / "Ryujinx.AppImage";
    if (std::filesystem::is_regular_file(managed_appimage)) {
        return true;
    }
    if (std::filesystem::is_regular_file(managed_binary)) {
        return true;
    }
    return find_source_binary().has_value();
}

std::string PosixRyujinxRuntime::unavailable_message() {
    const auto root = runtime_root();
    return "Ryujinx runtime not found. Place Ryujinx under " + root.string() +
           " (or set ARCHSTREAMER_RYUJINX). Link backends that need Ryujinx will stay unavailable.";
}

std::optional<ResolvedStandaloneEmulator> PosixRyujinxRuntime::ensure() {
    const auto root = runtime_root();
    const auto keys_dir = root / "keys";
    auto managed_binary = root / "Ryujinx";
    const auto managed_appimage = root / "Ryujinx.AppImage";
    std::filesystem::create_directories(root);
    std::filesystem::create_directories(keys_dir);

    if (!std::filesystem::exists(managed_binary) && !std::filesystem::exists(managed_appimage)) {
        const auto source = find_source_binary();
        if (!source.has_value()) {
            return std::nullopt;
        }
        const auto destination =
            (source->extension() == ".AppImage") ? managed_appimage : managed_binary;
        std::cout << "Installing Ryujinx into " << destination << " (from " << *source << ")\n";
        std::filesystem::copy_file(*source, destination, std::filesystem::copy_options::overwrite_existing);
        std::filesystem::permissions(
            destination,
            std::filesystem::perms::owner_all | std::filesystem::perms::group_read |
                std::filesystem::perms::group_exec | std::filesystem::perms::others_read |
                std::filesystem::perms::others_exec,
            std::filesystem::perm_options::replace);
        managed_binary = destination;
    }

    switch_copy_key_files(SwitchPaths::yuzu_runtime_root() / "keys", keys_dir);
    if (!std::filesystem::is_regular_file(keys_dir / "prod.keys")) {
        if (const auto source_keys = SwitchSystemDefaults::find_source_keys_dir(); source_keys.has_value()) {
            switch_copy_key_files(*source_keys, keys_dir);
        }
    }

    if (std::filesystem::is_regular_file(managed_appimage)) {
        return ResolvedStandaloneEmulator{managed_appimage, {"--fullscreen"}, "Ryujinx"};
    }
    if (std::filesystem::is_regular_file(managed_binary)) {
        return ResolvedStandaloneEmulator{managed_binary, {"--fullscreen"}, "Ryujinx"};
    }
    if (const auto source = find_source_binary(); source.has_value()) {
        return ResolvedStandaloneEmulator{*source, {"--fullscreen"}, "Ryujinx"};
    }
    return std::nullopt;
}

} // namespace archstreamer
