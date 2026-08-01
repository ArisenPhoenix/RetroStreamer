#include "common/platform/paths.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdlib>
#include <filesystem>
#include <string>

namespace archstreamer {
namespace {

std::string getenv_string(const char* name) {
    const char* value = std::getenv(name);
    return value == nullptr ? std::string{} : std::string(value);
}

} // namespace

std::string user_home_directory() {
    if (const auto profile = getenv_string("USERPROFILE"); !profile.empty()) {
        return profile;
    }
    if (const auto home = getenv_string("HOME"); !home.empty()) {
        return home;
    }
    return {};
}

std::string user_cache_directory() {
    if (const auto local = getenv_string("LOCALAPPDATA"); !local.empty()) {
        return local;
    }
    const auto home = user_home_directory();
    return home.empty() ? std::string{} : (std::filesystem::path(home) / "AppData" / "Local").string();
}

std::string archstreamer_cache_directory() {
    const auto cache = user_cache_directory();
    if (cache.empty()) {
        return {};
    }
    return (std::filesystem::path(cache) / "archstreamer").string();
}

std::string current_username() {
    if (const auto user = getenv_string("USERNAME"); !user.empty()) {
        return user;
    }
    char buffer[256]{};
    DWORD size = static_cast<DWORD>(sizeof(buffer));
    if (GetUserNameA(buffer, &size)) {
        return buffer;
    }
    return "player";
}

} // namespace archstreamer
