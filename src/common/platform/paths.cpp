#include "common/platform/paths.hpp"

#include <cstdlib>
#include <filesystem>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <pwd.h>
#include <unistd.h>
#endif

namespace archstreamer {
namespace {

std::string getenv_string(const char* name) {
    const char* value = std::getenv(name);
    return value == nullptr ? std::string{} : std::string(value);
}

} // namespace

std::string user_home_directory() {
#ifdef _WIN32
    if (const auto profile = getenv_string("USERPROFILE"); !profile.empty()) {
        return profile;
    }
    if (const auto home = getenv_string("HOME"); !home.empty()) {
        return home;
    }
    return {};
#else
    if (const auto home = getenv_string("HOME"); !home.empty()) {
        return home;
    }
    if (const auto* pw = getpwuid(getuid()); pw != nullptr && pw->pw_dir != nullptr) {
        return pw->pw_dir;
    }
    return {};
#endif
}

std::string user_cache_directory() {
#ifdef _WIN32
    if (const auto local = getenv_string("LOCALAPPDATA"); !local.empty()) {
        return local;
    }
    const auto home = user_home_directory();
    return home.empty() ? std::string{} : (std::filesystem::path(home) / "AppData" / "Local").string();
#else
    if (const auto xdg = getenv_string("XDG_CACHE_HOME"); !xdg.empty()) {
        return xdg;
    }
    const auto home = user_home_directory();
    return home.empty() ? std::string{} : (std::filesystem::path(home) / ".cache").string();
#endif
}

std::string archstreamer_cache_directory() {
    const auto cache = user_cache_directory();
    if (cache.empty()) {
        return {};
    }
    return (std::filesystem::path(cache) / "archstreamer").string();
}

std::string current_username() {
#ifdef _WIN32
    if (const auto user = getenv_string("USERNAME"); !user.empty()) {
        return user;
    }
    char buffer[256]{};
    DWORD size = static_cast<DWORD>(sizeof(buffer));
    if (GetUserNameA(buffer, &size)) {
        return buffer;
    }
    return "player";
#else
    if (const auto user = getenv_string("USER"); !user.empty()) {
        return user;
    }
    if (const auto* pw = getpwuid(getuid()); pw != nullptr && pw->pw_name != nullptr) {
        return pw->pw_name;
    }
    return "player";
#endif
}

} // namespace archstreamer
