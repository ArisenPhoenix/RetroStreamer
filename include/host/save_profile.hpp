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
    /** RetroArch system_directory for this user: mirror of the shared tree. */
    std::filesystem::path system_directory;
};

std::filesystem::path default_save_profile_root();
void copy_directory_contents(
    const std::filesystem::path& source,
    const std::filesystem::path& destination);
SaveProfile prepare_save_profile(
    const std::filesystem::path& root_directory,
    const std::string& username);

// Per-user LRPS2 memory cards live at <user>/pcsx2/memcards/*.ps2 and are reached
// through the user's system_directory mirror, so concurrent sessions never open
// the same card file.
std::filesystem::path user_ps2_memcard_directory(const SaveProfile& profile);

/** Shared BIOS / database tree the per-user mirrors link back to. */
std::filesystem::path shared_retroarch_system_directory();

/**
 * Refresh <user>/retroarch-system: symlinks to the shared system tree, except
 * pcsx2/memcards which resolves to this user's own cards. Returns the mirror.
 */
std::filesystem::path prepare_user_system_directory(const SaveProfile& profile);

} // namespace archstreamer
