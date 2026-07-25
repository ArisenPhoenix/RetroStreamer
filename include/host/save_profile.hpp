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

// Per-user LRPS2 memory cards live at <user>/pcsx2/memcards/*.ps2.
// Stage into ~/.config/retroarch/system/pcsx2/memcards before launch; harvest after.
std::filesystem::path user_ps2_memcard_directory(const SaveProfile& profile);
std::filesystem::path shared_ps2_memcard_directory();
void stage_user_ps2_memcards(const SaveProfile& profile);
void harvest_user_ps2_memcards(const SaveProfile& profile);

} // namespace archstreamer
