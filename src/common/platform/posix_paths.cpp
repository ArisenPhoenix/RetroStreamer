#include "common/platform/paths.hpp"

#include <cstdlib>
#include <filesystem>
#include <pwd.h>
#include <string>
#include <unistd.h>

namespace archstreamer {
namespace {

std::string getenv_string(const char* name) {
    const char* value = std::getenv(name);
    return value == nullptr ? std::string{} : std::string(value);
}

} // namespace

std::string user_home_directory() {
    if (const auto home = getenv_string("HOME"); !home.empty()) {
        return home;
    }
    if (const auto* pw = getpwuid(getuid()); pw != nullptr && pw->pw_dir != nullptr) {
        return pw->pw_dir;
    }
    return {};
}

std::string user_cache_directory() {
    if (const auto xdg = getenv_string("XDG_CACHE_HOME"); !xdg.empty()) {
        return xdg;
    }
    const auto home = user_home_directory();
    return home.empty() ? std::string{} : (std::filesystem::path(home) / ".cache").string();
}

std::string archstreamer_cache_directory() {
    const auto cache = user_cache_directory();
    if (cache.empty()) {
        return {};
    }
    return (std::filesystem::path(cache) / "archstreamer").string();
}

std::string current_username() {
    if (const auto user = getenv_string("USER"); !user.empty()) {
        return user;
    }
    if (const auto* pw = getpwuid(getuid()); pw != nullptr && pw->pw_name != nullptr) {
        return pw->pw_name;
    }
    return "player";
}

} // namespace archstreamer
