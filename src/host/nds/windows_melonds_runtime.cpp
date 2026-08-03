#include "host/nds/windows_melonds_runtime.hpp"

#include "host/standalone_emulator.hpp"
#include "host/switch/switch_fs.hpp"

#include <iostream>
#include <string>
#include <vector>

namespace archstreamer {

std::filesystem::path WindowsMelonDsRuntime::runtime_root() {
    return default_archstreamer_data_root() / "melonds";
}

std::optional<std::filesystem::path> WindowsMelonDsRuntime::find_source_binary() {
    if (const auto env = switch_path_from_env("ARCHSTREAMER_MELONDS"); env.has_value()) {
        if (std::filesystem::is_regular_file(*env)) {
            return env;
        }
        if (std::filesystem::is_directory(*env)) {
            for (const char* name : {"melonDS.exe", "melonds.exe", "melonDS", "melonds"}) {
                const auto exe = *env / name;
                if (std::filesystem::is_regular_file(exe)) {
                    return exe;
                }
            }
        }
    }

    const auto home = switch_home_dir();
    const std::vector<std::filesystem::path> candidates{
        home / "AppData/Local/archstreamer/melonds/melonDS.exe",
        runtime_root() / "melonDS.exe",
    };
    for (const auto& candidate : candidates) {
        if (std::filesystem::is_regular_file(candidate)) {
            return candidate;
        }
    }
    return std::nullopt;
}

bool WindowsMelonDsRuntime::available() {
    if (std::filesystem::is_regular_file(runtime_root() / "melonDS.exe")) {
        return true;
    }
    return find_source_binary().has_value();
}

std::string WindowsMelonDsRuntime::unavailable_message() {
    return "melonDS runtime not found. Place melonDS.exe under " + runtime_root().string() +
           " (or set ARCHSTREAMER_MELONDS). NDS will use RetroArch until then.";
}

std::optional<ResolvedStandaloneEmulator> WindowsMelonDsRuntime::ensure() {
    const auto root = runtime_root();
    const auto managed_binary = root / "melonDS.exe";
    std::filesystem::create_directories(root);

    if (!std::filesystem::is_regular_file(managed_binary)) {
        const auto source = find_source_binary();
        if (!source.has_value()) {
            return std::nullopt;
        }
        std::cout << "Installing melonDS into " << managed_binary << " (from " << *source << ")\n";
        std::filesystem::copy_file(
            *source, managed_binary, std::filesystem::copy_options::overwrite_existing);
    }

    if (!std::filesystem::is_regular_file(managed_binary)) {
        return std::nullopt;
    }

    return ResolvedStandaloneEmulator{managed_binary, {}, "melonDS"};
}

} // namespace archstreamer
