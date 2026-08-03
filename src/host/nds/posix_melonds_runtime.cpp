#include "host/nds/posix_melonds_runtime.hpp"

#include "host/standalone_emulator.hpp"
#include "host/switch/switch_fs.hpp"

#include <iostream>
#include <string>
#include <vector>

namespace archstreamer {

std::filesystem::path PosixMelonDsRuntime::runtime_root() {
    return default_archstreamer_data_root() / "melonds";
}

std::optional<std::filesystem::path> PosixMelonDsRuntime::find_source_binary() {
    if (const auto env = switch_path_from_env("ARCHSTREAMER_MELONDS"); env.has_value()) {
        if (std::filesystem::is_regular_file(*env)) {
            return env;
        }
        if (std::filesystem::is_directory(*env)) {
            for (const char* name : {"melonDS", "melonds", "melonDS.AppImage", "melonds.AppImage"}) {
                const auto exe = *env / name;
                if (std::filesystem::is_regular_file(exe)) {
                    return exe;
                }
            }
        }
    }

    const auto home = switch_home_dir();
    const std::vector<std::filesystem::path> candidates{
        "/srv/emus/melonDS",
        "/srv/emus/melonDS.AppImage",
        "/srv/emus/melonds",
        home / "Applications/melonDS",
        home / "Applications/melonDS.AppImage",
        home / ".local/bin/melonDS",
        home / ".local/bin/melonds",
        "/usr/local/bin/melonDS",
        "/usr/local/bin/melonds",
        "/usr/bin/melonDS",
        "/usr/bin/melonds",
    };
    for (const auto& candidate : candidates) {
        if (std::filesystem::is_regular_file(candidate)) {
            return candidate;
        }
    }
    return std::nullopt;
}

bool PosixMelonDsRuntime::available() {
    const auto managed = runtime_root() / "melonDS";
    if (std::filesystem::is_regular_file(managed)) {
        return true;
    }
    const auto managed_appimage = runtime_root() / "melonDS.AppImage";
    if (std::filesystem::is_regular_file(managed_appimage)) {
        return true;
    }
    return find_source_binary().has_value();
}

std::string PosixMelonDsRuntime::unavailable_message() {
    return "melonDS runtime not found. Place melonDS under /srv/emus/melonDS "
           "(or " +
           runtime_root().string() +
           ", or set ARCHSTREAMER_MELONDS). NDS will use RetroArch until then.";
}

std::optional<ResolvedStandaloneEmulator> PosixMelonDsRuntime::ensure() {
    const auto root = runtime_root();
    const auto managed_binary = root / "melonDS";
    std::filesystem::create_directories(root);

    if (!std::filesystem::is_regular_file(managed_binary)) {
        const auto source = find_source_binary();
        if (!source.has_value()) {
            return std::nullopt;
        }
        // Prefer a stable managed name even when the source is an AppImage.
        std::cout << "Installing melonDS into " << managed_binary << " (from " << *source << ")\n";
        std::filesystem::copy_file(
            *source, managed_binary, std::filesystem::copy_options::overwrite_existing);
        std::filesystem::permissions(
            managed_binary,
            std::filesystem::perms::owner_all | std::filesystem::perms::group_read |
                std::filesystem::perms::group_exec | std::filesystem::perms::others_read |
                std::filesystem::perms::others_exec,
            std::filesystem::perm_options::replace);
    }

    if (!std::filesystem::is_regular_file(managed_binary)) {
        return std::nullopt;
    }

    return ResolvedStandaloneEmulator{
        managed_binary,
        {},
        "melonDS",
    };
}

} // namespace archstreamer
