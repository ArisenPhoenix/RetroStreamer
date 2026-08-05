#include "common/dlc_paths.hpp"

#include "common/catalog_paths.hpp"

#include <cctype>
#include <cstdlib>

namespace archstreamer {
namespace {

std::string to_lower_copy(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (char ch : value) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    return out;
}

std::filesystem::path archstreamer_data_root() {
#if defined(_WIN32)
    if (const char* local = std::getenv("LOCALAPPDATA"); local != nullptr && *local != '\0') {
        return std::filesystem::path{local} / "archstreamer";
    }
    return std::filesystem::path{"archstreamer"};
#else
    if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg != nullptr && *xdg != '\0') {
        return std::filesystem::path{xdg} / "archstreamer";
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        return std::filesystem::path{home} / ".local" / "share" / "archstreamer";
    }
    return std::filesystem::path{".local/share/archstreamer"};
#endif
}

} // namespace

std::filesystem::path default_dlc_root_for(const std::filesystem::path& rom_root) {
    if (rom_root.empty()) {
        return {};
    }
    return rom_root.parent_path() / "DLC";
}

std::filesystem::path resolve_dlc_root(const std::filesystem::path& rom_root) {
    if (const char* env = std::getenv("ARCHSTREAMER_DLC_ROOT");
        env != nullptr && *env != '\0') {
        return env;
    }
    if (!rom_root.empty()) {
        return default_dlc_root_for(rom_root);
    }
    if (const char* gaming = std::getenv("ARCHSTREAMER_GAMING_ROOT");
        gaming != nullptr && *gaming != '\0') {
        return std::filesystem::path{gaming} / RelDlcRoot;
    }
    const std::filesystem::path roms{"/mnt/Internal_SSD/Gaming/ROMS"};
    if (std::filesystem::is_directory(roms / "Games") ||
        std::filesystem::is_directory(roms / "SwitchUpdates") ||
        std::filesystem::is_directory(roms / "DLC")) {
        return roms / "DLC";
    }
    return archstreamer_data_root() / "system" / "dlc";
}

std::string dlc_system_folder_name(std::string_view system_key) {
    const auto key = to_lower_copy(system_key);
    if (key == "switch" || key == "nx") {
        return "Switch";
    }
    if (key == "snes" || key == "sfc") {
        return "SNES";
    }
    if (key == "nes" || key == "famicom") {
        return "NES";
    }
    if (key == "n64") {
        return "N64";
    }
    if (key == "nds" || key == "ds") {
        return "NDS";
    }
    if (key == "3ds") {
        return "3DS";
    }
    if (key == "gb") {
        return "GB";
    }
    if (key == "gbc") {
        return "GBC";
    }
    if (key == "gba") {
        return "GBA";
    }
    if (key == "gamecube" || key == "gc" || key == "ngc") {
        return "GameCube";
    }
    if (key == "wii") {
        return "Wii";
    }
    if (key == "ps1" || key == "psx") {
        return "PS1";
    }
    if (key == "ps2") {
        return "PS2";
    }
    if (key == "psp") {
        return "PSP";
    }
    if (key.empty()) {
        return "Other";
    }
    std::string out(system_key);
    if (!out.empty()) {
        out[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(out[0])));
    }
    return out;
}

std::filesystem::path catalog_dlc_game_directory(
    const std::filesystem::path& dlc_root,
    std::string_view system_key,
    std::string_view content_stem) {
    if (dlc_root.empty() || content_stem.empty()) {
        return {};
    }
    return dlc_root / dlc_system_folder_name(system_key) / std::string(content_stem);
}

std::filesystem::path switch_dlc_game_directory(std::string_view content_stem) {
    return catalog_dlc_game_directory(resolve_dlc_root(), "switch", content_stem);
}

std::filesystem::path legacy_switch_updates_directory() {
    if (const char* env = std::getenv("ARCHSTREAMER_SWITCH_UPDATES");
        env != nullptr && *env != '\0') {
        return env;
    }
    const std::filesystem::path gaming{"/mnt/Internal_SSD/Gaming/ROMS/SwitchUpdates"};
    if (std::filesystem::is_directory(gaming)) {
        return gaming;
    }
    return archstreamer_data_root() / "system" / "updates";
}

} // namespace archstreamer
