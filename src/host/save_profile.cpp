#include "host/save_profile.hpp"

#include <cstdlib>
#include <stdexcept>
#include <string_view>
#include <system_error>

namespace archstreamer {

std::filesystem::path default_save_profile_root() {
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        return std::filesystem::path(home) / ".local/share/archstreamer/saves";
    }

    return std::filesystem::current_path() / "archstreamer-saves";
}

void copy_directory_contents(
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

SaveProfile prepare_save_profile(
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
    // Per-user Yuzu / Ryujinx isolation + shared Switch save leaves.
    std::filesystem::create_directories(template_directory / "yuzu" / "xdg-data" / "yuzu" / "keys");
    std::filesystem::create_directories(template_directory / "yuzu" / "xdg-config" / "yuzu");
    std::filesystem::create_directories(template_directory / "ryujinx" / "xdg-config" / "Ryujinx" / "system");
    std::filesystem::create_directories(template_directory / "ryujinx" / "xdg-config" / "Ryujinx" / "bis" / "user" / "save");
    std::filesystem::create_directories(template_directory / "switch" / "saves");

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
    std::filesystem::create_directories(user_ps2_memcard_directory(profile));
    profile.system_directory = prepare_user_system_directory(profile);
    return profile;
}

std::filesystem::path user_ps2_memcard_directory(const SaveProfile& profile) {
    return profile.user_directory / "pcsx2" / "memcards";
}

std::filesystem::path shared_retroarch_system_directory() {
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        return std::filesystem::path(home) / ".config/retroarch/system";
    }
    return std::filesystem::path(".config/retroarch/system");
}

namespace {

void link_into_mirror(
    const std::filesystem::path& link,
    const std::filesystem::path& target) {
    std::error_code error;
    if (std::filesystem::is_symlink(link, error)) {
        if (std::filesystem::read_symlink(link, error) == target) {
            return;
        }
        std::filesystem::remove(link, error);
    } else if (std::filesystem::exists(link, error)) {
        // Something real already sits there; never clobber a user's own files.
        return;
    }
    std::filesystem::create_symlink(target, link, error);
}

void mirror_children(
    const std::filesystem::path& source,
    const std::filesystem::path& mirror,
    std::string_view skip) {
    std::error_code error;
    if (!std::filesystem::is_directory(source, error)) {
        return;
    }
    for (const auto& entry : std::filesystem::directory_iterator(source, error)) {
        const auto name = entry.path().filename().string();
        if (name == skip) {
            continue;
        }
        link_into_mirror(mirror / name, entry.path());
    }
}

} // namespace

std::filesystem::path prepare_user_system_directory(const SaveProfile& profile) {
    const auto shared_system = shared_retroarch_system_directory();
    const auto mirror = profile.user_directory / "retroarch-system";
    std::error_code error;
    std::filesystem::create_directories(mirror, error);

    // BIOS and databases stay shared; only the PS2 tree needs per-user identity.
    mirror_children(shared_system, mirror, "pcsx2");

    const auto pcsx2 = mirror / "pcsx2";
    std::filesystem::create_directories(pcsx2, error);
    mirror_children(shared_system / "pcsx2", pcsx2, "memcards");

    const auto cards = user_ps2_memcard_directory(profile);
    std::filesystem::create_directories(cards, error);
    link_into_mirror(pcsx2 / "memcards", cards);
    return mirror;
}

} // namespace archstreamer
