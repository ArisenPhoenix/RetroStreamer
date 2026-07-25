#include "host/save_profile.hpp"

#include <cstdlib>
#include <stdexcept>

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
    // Per-user Yuzu isolation (XDG roots seeded at launch via prepare_yuzu_user_profile).
    std::filesystem::create_directories(template_directory / "yuzu" / "xdg-data" / "yuzu" / "keys");
    std::filesystem::create_directories(template_directory / "yuzu" / "xdg-config" / "yuzu");

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
    return profile;
}

std::filesystem::path user_ps2_memcard_directory(const SaveProfile& profile) {
    return profile.user_directory / "pcsx2" / "memcards";
}

std::filesystem::path shared_ps2_memcard_directory() {
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        return std::filesystem::path(home) / ".config/retroarch/system/pcsx2/memcards";
    }
    return std::filesystem::path(".config/retroarch/system/pcsx2/memcards");
}

namespace {

void copy_ps2_memcards(
    const std::filesystem::path& source_dir,
    const std::filesystem::path& dest_dir) {
    if (!std::filesystem::exists(source_dir)) {
        return;
    }
    std::filesystem::create_directories(dest_dir);
    for (const auto& entry : std::filesystem::directory_iterator(source_dir)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        const auto name = entry.path().filename().string();
        if (name.size() < 4 || name.substr(name.size() - 4) != ".ps2") {
            continue;
        }
        const auto target = dest_dir / entry.path().filename();
        std::filesystem::copy_file(
            entry.path(),
            target,
            std::filesystem::copy_options::overwrite_existing);
    }
}

} // namespace

void stage_user_ps2_memcards(const SaveProfile& profile) {
    const auto user_cards = user_ps2_memcard_directory(profile);
    const auto shared_cards = shared_ps2_memcard_directory();
    std::filesystem::create_directories(user_cards);
    std::filesystem::create_directories(shared_cards);

    // First PS2 session for this user: seed from the shared RetroArch cards if empty.
    bool user_has_card = false;
    if (std::filesystem::exists(user_cards)) {
        for (const auto& entry : std::filesystem::directory_iterator(user_cards)) {
            if (entry.is_regular_file() && entry.path().extension() == ".ps2") {
                user_has_card = true;
                break;
            }
        }
    }
    if (!user_has_card) {
        copy_ps2_memcards(shared_cards, user_cards);
    }
    copy_ps2_memcards(user_cards, shared_cards);
}

void harvest_user_ps2_memcards(const SaveProfile& profile) {
    copy_ps2_memcards(shared_ps2_memcard_directory(), user_ps2_memcard_directory(profile));
}

} // namespace archstreamer
