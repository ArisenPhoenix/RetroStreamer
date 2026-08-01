#include "host/switch/posix_yuzu_runtime.hpp"

#include "host/switch/default_switch_paths.hpp"
#include "host/switch/switch_fs.hpp"
#include "host/switch/switch_system_defaults.hpp"

#include <iostream>
#include <string>
#include <vector>

namespace archstreamer {

std::filesystem::path PosixYuzuRuntime::runtime_root() {
    return SwitchPaths::yuzu_runtime_root();
}

std::optional<std::filesystem::path> PosixYuzuRuntime::find_source_binary() {
    if (const auto env = switch_path_from_env("ARCHSTREAMER_YUZU"); env.has_value()) {
        if (std::filesystem::is_regular_file(*env)) {
            return env;
        }
    }
    if (const auto env = switch_path_from_env("YUZU_APPIMAGE"); env.has_value()) {
        if (std::filesystem::is_regular_file(*env)) {
            return env;
        }
    }
    if (const auto env = switch_path_from_env("YUZU_BIN"); env.has_value()) {
        if (std::filesystem::is_regular_file(*env)) {
            return env;
        }
    }

    const auto home = switch_home_dir();
    const std::vector<std::filesystem::path> candidates{
        "/srv/emus/Switch/yuzu.AppImage",
        "/srv/emus/Switch/yuzu-20231111-030c140c0.AppImage",
        home / "Applications/yuzu.AppImage",
        home / ".local/bin/yuzu",
        "/usr/local/bin/yuzu",
        "/usr/bin/yuzu",
    };

    for (const auto& candidate : candidates) {
        if (!std::filesystem::exists(candidate)) {
            continue;
        }
        if (std::filesystem::is_regular_file(candidate)) {
            const auto name = candidate.filename().string();
            if (candidate.extension() == ".AppImage" || name == "yuzu" ||
                name.find("yuzu") != std::string::npos) {
                return candidate;
            }
            return candidate;
        }
        if (std::filesystem::is_directory(candidate)) {
            for (const auto& entry : std::filesystem::directory_iterator(candidate)) {
                if (!entry.is_regular_file()) {
                    continue;
                }
                const auto path = entry.path();
                if (path.extension() == ".AppImage" || path.filename() == "yuzu") {
                    return path;
                }
            }
        }
    }
    return std::nullopt;
}

bool PosixYuzuRuntime::available() {
    const auto managed_binary = runtime_root() / "yuzu.AppImage";
    if (std::filesystem::is_regular_file(managed_binary)) {
        return true;
    }
    return find_source_binary().has_value();
}

std::string PosixYuzuRuntime::unavailable_message() {
    const auto root = runtime_root();
    return "Yuzu runtime not found. Switch titles require yuzu.AppImage under " + root.string() +
           " (or set ARCHSTREAMER_YUZU). "
           "Nintendo Switch games will not be listed until Yuzu is installed.";
}

std::optional<ResolvedStandaloneEmulator> PosixYuzuRuntime::ensure() {
    const auto root = runtime_root();
    const auto managed_binary = root / "yuzu.AppImage";
    const auto managed_keys = root / "keys";
    std::filesystem::create_directories(root);
    std::filesystem::create_directories(managed_keys);

    if (!std::filesystem::exists(managed_binary)) {
        const auto source = find_source_binary();
        if (!source.has_value()) {
            return std::nullopt;
        }
        std::cout << "Installing Yuzu AppImage into " << managed_binary << " (from " << *source << ")\n";
        std::filesystem::copy_file(*source, managed_binary, std::filesystem::copy_options::overwrite_existing);
        std::filesystem::permissions(
            managed_binary,
            std::filesystem::perms::owner_all | std::filesystem::perms::group_read |
                std::filesystem::perms::group_exec | std::filesystem::perms::others_read |
                std::filesystem::perms::others_exec,
            std::filesystem::perm_options::replace);
    }

    if (!std::filesystem::exists(managed_keys / "prod.keys")) {
        if (const auto source_keys = SwitchSystemDefaults::find_source_keys_dir(); source_keys.has_value()) {
            std::cout << "Installing Yuzu keys into " << managed_keys << " (from " << *source_keys << ")\n";
            switch_copy_key_files(*source_keys, managed_keys);
        } else {
            std::cerr << "Warning: Yuzu prod.keys not found. Place keys in " << managed_keys
                      << " or set ARCHSTREAMER_YUZU_KEYS.\n";
        }
    } else if (const auto source_keys = SwitchSystemDefaults::find_source_keys_dir(); source_keys.has_value()) {
        switch_copy_key_files(*source_keys, managed_keys);
    }

    if (!std::filesystem::is_regular_file(managed_binary)) {
        return std::nullopt;
    }

    return ResolvedStandaloneEmulator{
        managed_binary,
        {"-f", "-g"},
        "Yuzu",
    };
}

} // namespace archstreamer
