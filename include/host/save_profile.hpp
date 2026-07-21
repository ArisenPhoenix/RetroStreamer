#pragma once

#include "common/protocol.hpp"

#include <filesystem>
#include <string>

namespace archstreamer {

struct SaveProfile {
    std::string username;
    std::filesystem::path root_directory;
    std::filesystem::path user_directory;
    std::filesystem::path savefile_directory;
    std::filesystem::path savestate_directory;
};

std::filesystem::path default_save_profile_root();
void copy_directory_contents(
    const std::filesystem::path& source,
    const std::filesystem::path& destination);
SaveProfile prepare_save_profile(
    const std::filesystem::path& root_directory,
    const std::string& username);

} // namespace archstreamer
