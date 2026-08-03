#include "host/retroarch_resolve.hpp"

#include "common/platform/paths.hpp"

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace archstreamer {
namespace {

bool is_regular_executable(const std::filesystem::path& path) {
    std::error_code error;
    return !path.empty() && std::filesystem::is_regular_file(path, error);
}

std::filesystem::path first_existing(const std::vector<std::filesystem::path>& candidates) {
    for (const auto& candidate : candidates) {
        if (is_regular_executable(candidate)) {
            return candidate;
        }
    }
    return {};
}

std::vector<std::filesystem::path> native_retroarch_candidates() {
    std::vector<std::filesystem::path> out;
    if (const char* env = std::getenv("ARCHSTREAMER_RETROARCH");
        env != nullptr && *env != '\0') {
        out.emplace_back(env);
    }
    if (const char* env = std::getenv("RETROARCH"); env != nullptr && *env != '\0') {
        out.emplace_back(env);
    }

    const auto home = user_home_directory();
    const auto local = [&]() -> std::filesystem::path {
        if (const char* v = std::getenv("LOCALAPPDATA"); v != nullptr && *v != '\0') {
            return v;
        }
        return home.empty() ? std::filesystem::path{}
                            : std::filesystem::path(home) / "AppData" / "Local";
    }();
    const auto program_files = [&]() -> std::filesystem::path {
        if (const char* v = std::getenv("ProgramFiles"); v != nullptr && *v != '\0') {
            return v;
        }
        return "C:/Program Files";
    }();
    const auto program_files_x86 = [&]() -> std::filesystem::path {
        if (const char* v = std::getenv("ProgramFiles(x86)"); v != nullptr && *v != '\0') {
            return v;
        }
        return "C:/Program Files (x86)";
    }();

    if (!local.empty()) {
        out.push_back(local / "Programs" / "RetroArch" / "retroarch.exe");
        out.push_back(local / "archstreamer" / "retroarch" / "retroarch.exe");
    }
    if (!home.empty()) {
        out.push_back(std::filesystem::path(home) / "scoop" / "apps" / "retroarch" / "current" /
                      "retroarch.exe");
    }
    out.push_back(program_files / "RetroArch" / "retroarch.exe");
    out.push_back(program_files_x86 / "RetroArch" / "retroarch.exe");
    out.emplace_back("retroarch.exe");
    return out;
}

std::filesystem::path which_retroarch() {
    char buffer[MAX_PATH]{};
    const DWORD n = SearchPathA(nullptr, "retroarch.exe", nullptr, MAX_PATH, buffer, nullptr);
    if (n > 0 && n < MAX_PATH) {
        return buffer;
    }
    return {};
}

} // namespace

ResolvedRetroArch resolve_retroarch() {
    if (const auto path = first_existing(native_retroarch_candidates()); !path.empty()) {
        return ResolvedRetroArch{{path.string()}, path.string()};
    }
    if (const auto path = which_retroarch(); !path.empty()) {
        return ResolvedRetroArch{{path.string()}, path.string()};
    }
    throw std::runtime_error(
        "RetroArch not found. Install RetroArch, add retroarch.exe to PATH, "
        "or set ARCHSTREAMER_RETROARCH to the full path.");
}

} // namespace archstreamer

#endif // _WIN32
