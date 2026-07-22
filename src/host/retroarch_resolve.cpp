#include "host/retroarch_resolve.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>

namespace archstreamer {
namespace {

bool is_executable(const std::filesystem::path& path) {
    return !path.empty() && access(path.c_str(), X_OK) == 0;
}

bool running_inside_flatpak() {
    return access("/.flatpak-info", F_OK) == 0;
}

std::optional<std::filesystem::path> first_executable(const std::vector<std::filesystem::path>& candidates) {
    for (const auto& candidate : candidates) {
        if (is_executable(candidate)) {
            return candidate;
        }
    }
    return std::nullopt;
}

bool command_succeeds(const std::string& command) {
    const int status = std::system(command.c_str());
    return status == 0;
}

bool looks_like_retroarch(const std::filesystem::path& path) {
    // Reject empty / non-executables early. Avoid spawning a GUI window: --help is enough.
    const auto command = std::string("\"") + path.string() + "\" --help >/dev/null 2>&1";
    return command_succeeds(command);
}

std::vector<std::filesystem::path> native_retroarch_candidates() {
    const char* home = std::getenv("HOME");
    std::vector<std::filesystem::path> native{
        "/usr/bin/retroarch",
        "/usr/local/bin/retroarch",
    };
    if (home != nullptr) {
        native.emplace_back(std::filesystem::path(home) / ".local/bin/retroarch");
    }
    return native;
}

std::vector<std::filesystem::path> flatpak_export_candidates() {
    const char* home = std::getenv("HOME");
    std::vector<std::filesystem::path> exports;
    if (home != nullptr) {
        exports.emplace_back(
            std::filesystem::path(home) / ".local/share/flatpak/exports/bin/org.libretro.RetroArch");
    }
    exports.emplace_back("/var/lib/flatpak/exports/bin/org.libretro.RetroArch");
    return exports;
}

} // namespace

ResolvedRetroArch resolve_retroarch() {
    // Inside our own Flatpak sandbox, /usr/bin/retroarch is the runtime — not the host binary
    // `which retroarch` sees in a normal terminal. Prefer escaping to the host.
    if (running_inside_flatpak()) {
        if (command_succeeds("flatpak-spawn --host which retroarch >/dev/null 2>&1")) {
            return ResolvedRetroArch{
                {"flatpak-spawn", "--host", "retroarch"},
                "flatpak-spawn --host retroarch",
            };
        }
        if (command_succeeds("flatpak-spawn --host flatpak info org.libretro.RetroArch >/dev/null 2>&1")) {
            return ResolvedRetroArch{
                {"flatpak-spawn", "--host", "flatpak", "run", "org.libretro.RetroArch"},
                "flatpak-spawn --host flatpak run org.libretro.RetroArch",
            };
        }
    }

    if (const auto path = first_executable(native_retroarch_candidates()); path.has_value()) {
        if (looks_like_retroarch(*path)) {
            return ResolvedRetroArch{{path->string()}, path->string()};
        }
    }

    if (const auto path = first_executable(flatpak_export_candidates()); path.has_value()) {
        return ResolvedRetroArch{{path->string()}, path->string()};
    }

    if (command_succeeds("flatpak info org.libretro.RetroArch >/dev/null 2>&1")) {
        return ResolvedRetroArch{
            {"flatpak", "run", "org.libretro.RetroArch"},
            "flatpak run org.libretro.RetroArch",
        };
    }

    throw std::runtime_error(
        "RetroArch not found or not runnable from this process. "
        "In a terminal, check `which retroarch` and `retroarch --help`. "
        "If ArchStreamer is a Flatpak, host launch needs host RetroArch via flatpak-spawn.");
}

} // namespace archstreamer
