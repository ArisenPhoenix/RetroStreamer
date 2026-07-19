#pragma once

#include "common/protocol.hpp"

#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>

namespace archstreamer {

struct SaveProfile {
    std::string username;
    std::filesystem::path root_directory;
    std::filesystem::path user_directory;
    std::filesystem::path savefile_directory;
    std::filesystem::path savestate_directory;
};

inline std::filesystem::path default_save_profile_root() {
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        return std::filesystem::path(home) / ".local/share/archstreamer/saves";
    }

    return std::filesystem::current_path() / "archstreamer-saves";
}

inline void copy_directory_contents(
    const std::filesystem::path& source,
    const std::filesystem::path& destination) {
    if (!std::filesystem::exists(source)) {
        return;
    }
    if (!std::filesystem::is_directory(source)) {
        throw std::runtime_error("save template path exists but is not a directory");
    }

    for (const auto& entry : std::filesystem::recursive_directory_iterator(source)) {
        const auto relative = std::filesystem::relative(entry.path(), source);
        const auto target = destination / relative;

        if (entry.is_directory()) {
            std::filesystem::create_directories(target);
        } else if (entry.is_regular_file()) {
            std::filesystem::create_directories(target.parent_path());
            std::filesystem::copy_file(entry.path(), target, std::filesystem::copy_options::skip_existing);
        }
    }
}

inline SaveProfile prepare_save_profile(
    const std::filesystem::path& root_directory,
    const std::string& username) {
    if (!valid_username(username)) {
        throw std::runtime_error("invalid save profile username");
    }

    const auto template_directory = root_directory / "template";
    const auto user_directory = root_directory / username;
    const bool new_user = !std::filesystem::exists(user_directory);

    std::filesystem::create_directories(root_directory);
    std::filesystem::create_directories(template_directory / "saves");
    std::filesystem::create_directories(template_directory / "states");

    if (new_user) {
        std::filesystem::create_directories(user_directory);
        copy_directory_contents(template_directory, user_directory);
    }

    SaveProfile profile;
    profile.username = username;
    profile.root_directory = root_directory;
    profile.user_directory = user_directory;
    profile.savefile_directory = user_directory / "saves";
    profile.savestate_directory = user_directory / "states";

    std::filesystem::create_directories(profile.savefile_directory);
    std::filesystem::create_directories(profile.savestate_directory);
    return profile;
}

} // namespace archstreamer
