#include "host/switch/windows_ryujinx_runtime.hpp"

#include "host/switch/default_switch_paths.hpp"
#include "host/switch/switch_fs.hpp"
#include "host/switch/switch_system_defaults.hpp"

#include "common/platform/paths.hpp"

#include <iostream>
#include <vector>

namespace archstreamer {

std::filesystem::path WindowsRyujinxRuntime::runtime_root() {
    return SwitchPaths::ryujinx_runtime_root();
}

std::optional<std::filesystem::path> WindowsRyujinxRuntime::find_source_binary() {
    if (const auto env = switch_path_from_env("ARCHSTREAMER_RYUJINX"); env.has_value()) {
        if (std::filesystem::is_regular_file(*env)) {
            return env;
        }
        if (std::filesystem::is_directory(*env)) {
            for (const char* name : {"Ryujinx.exe", "ryujinx.exe"}) {
                const auto exe = *env / name;
                if (std::filesystem::is_regular_file(exe)) {
                    return exe;
                }
            }
        }
    }

    const auto home = switch_home_dir();
    const auto local = archstreamer_cache_directory();
    const std::vector<std::filesystem::path> candidates{
        std::filesystem::path{local} / "ryujinx" / "Ryujinx.exe",
        home / "Downloads" / "Ryujinx" / "Ryujinx.exe",
        home / "AppData" / "Local" / "Ryujinx" / "Ryujinx.exe",
    };
    for (const auto& candidate : candidates) {
        if (std::filesystem::is_regular_file(candidate)) {
            return candidate;
        }
    }
    return std::nullopt;
}

bool WindowsRyujinxRuntime::available() {
    const auto managed_binary = runtime_root() / "Ryujinx.exe";
    if (std::filesystem::is_regular_file(managed_binary)) {
        return true;
    }
    return find_source_binary().has_value();
}

std::string WindowsRyujinxRuntime::unavailable_message() {
    const auto root = runtime_root();
    return "Ryujinx runtime not found. Place Ryujinx.exe under " + root.string() +
           " (or set ARCHSTREAMER_RYUJINX). Link backends that need Ryujinx will stay unavailable.";
}

std::optional<ResolvedStandaloneEmulator> WindowsRyujinxRuntime::ensure() {
    const auto root = runtime_root();
    const auto keys_dir = root / "keys";
    const auto managed_binary = root / "Ryujinx.exe";
    std::filesystem::create_directories(root);
    std::filesystem::create_directories(keys_dir);

    if (!std::filesystem::exists(managed_binary)) {
        const auto source = find_source_binary();
        if (!source.has_value()) {
            return std::nullopt;
        }
        const auto source_dir = source->parent_path();
        std::cout << "Installing Ryujinx Windows build into " << root << " (from " << source_dir << ")\n";
        for (const auto& entry : std::filesystem::directory_iterator(source_dir)) {
            if (!entry.is_regular_file() && !entry.is_directory()) {
                continue;
            }
            const auto name = entry.path().filename();
            if (entry.is_directory()) {
                std::filesystem::copy(
                    entry.path(),
                    root / name,
                    std::filesystem::copy_options::recursive |
                        std::filesystem::copy_options::skip_existing);
            } else {
                std::filesystem::copy_file(
                    entry.path(),
                    root / name,
                    std::filesystem::copy_options::skip_existing);
            }
        }
    }

    switch_copy_key_files(SwitchPaths::yuzu_runtime_root() / "keys", keys_dir);
    if (!std::filesystem::is_regular_file(keys_dir / "prod.keys")) {
        if (const auto source_keys = SwitchSystemDefaults::find_source_keys_dir(); source_keys.has_value()) {
            switch_copy_key_files(*source_keys, keys_dir);
        }
    }

    if (!std::filesystem::is_regular_file(managed_binary)) {
        return std::nullopt;
    }
    return ResolvedStandaloneEmulator{managed_binary, {"--fullscreen"}, "Ryujinx"};
}

} // namespace archstreamer
