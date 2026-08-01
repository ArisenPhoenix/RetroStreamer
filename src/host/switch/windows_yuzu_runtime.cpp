#include "host/switch/windows_yuzu_runtime.hpp"

#include "host/switch/default_switch_paths.hpp"
#include "host/switch/switch_fs.hpp"
#include "host/switch/switch_system_defaults.hpp"

#include "common/platform/paths.hpp"

#include <iostream>
#include <vector>

namespace archstreamer {

std::filesystem::path WindowsYuzuRuntime::runtime_root() {
    return SwitchPaths::yuzu_runtime_root();
}

std::optional<std::filesystem::path> WindowsYuzuRuntime::find_source_binary() {
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
    const auto local = archstreamer_cache_directory();
    const std::vector<std::filesystem::path> candidates{
        std::filesystem::path{local} / "yuzu" / "yuzu.exe",
        home / "Downloads" / "Yuzu_WIN" / "yuzu-windows-msvc" / "yuzu.exe",
        home / "Downloads" / "yuzu-windows-msvc" / "yuzu.exe",
        home / "AppData" / "Local" / "yuzu" / "yuzu.exe",
    };
    for (const auto& candidate : candidates) {
        if (std::filesystem::is_regular_file(candidate)) {
            return candidate;
        }
        if (std::filesystem::is_directory(candidate.parent_path())) {
            const auto exe = candidate.parent_path() / "yuzu.exe";
            if (std::filesystem::is_regular_file(exe)) {
                return exe;
            }
        }
    }
    if (const auto env = switch_path_from_env("ARCHSTREAMER_YUZU"); env.has_value()) {
        if (std::filesystem::is_directory(*env)) {
            for (const char* name : {"yuzu.exe", "yuzu-cmd.exe"}) {
                const auto exe = *env / name;
                if (std::filesystem::is_regular_file(exe)) {
                    return exe;
                }
            }
        }
    }
    return std::nullopt;
}

bool WindowsYuzuRuntime::available() {
    const auto managed_binary = runtime_root() / "yuzu.exe";
    if (std::filesystem::is_regular_file(managed_binary)) {
        return true;
    }
    return find_source_binary().has_value();
}

std::string WindowsYuzuRuntime::unavailable_message() {
    const auto root = runtime_root();
    return "Yuzu runtime not found. Switch titles require yuzu.exe under " + root.string() +
           " (or set ARCHSTREAMER_YUZU to a yuzu-windows-msvc build). "
           "Nintendo Switch games will not be listed until Yuzu is installed.";
}

std::optional<ResolvedStandaloneEmulator> WindowsYuzuRuntime::ensure() {
    const auto root = runtime_root();
    const auto managed_binary = root / "yuzu.exe";
    const auto managed_keys = root / "keys";
    std::filesystem::create_directories(root);
    std::filesystem::create_directories(managed_keys);

    if (!std::filesystem::exists(managed_binary)) {
        const auto source = find_source_binary();
        if (!source.has_value()) {
            return std::nullopt;
        }
        const auto source_dir = source->parent_path();
        std::cout << "Installing Yuzu Windows build into " << root << " (from " << source_dir << ")\n";
        for (const auto& entry : std::filesystem::directory_iterator(source_dir)) {
            const auto name = entry.path().filename();
            if (name == "plugins" || name == "mediaservice") {
                std::filesystem::copy(
                    entry.path(),
                    root / name,
                    std::filesystem::copy_options::recursive |
                        std::filesystem::copy_options::overwrite_existing);
                continue;
            }
            if (!entry.is_regular_file()) {
                continue;
            }
            std::filesystem::copy_file(
                entry.path(),
                root / name,
                std::filesystem::copy_options::overwrite_existing);
        }
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
