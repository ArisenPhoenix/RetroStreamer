#include "host/libretro_core_registry.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <utility>

namespace archstreamer {

LibretroCoreRegistry LibretroCoreRegistry::defaults() {
    return LibretroCoreRegistry(default_core_dirs());
}

LibretroCoreRegistry LibretroCoreRegistry::ubuntu_defaults() {
    return defaults();
}

LibretroCoreRegistry::LibretroCoreRegistry(std::vector<std::filesystem::path> core_dirs)
    : core_dirs_(std::move(core_dirs)) {
    add_system("gb", "Game Boy", {{"gambatte", "Gambatte"}, {"sameboy", "SameBoy"}});
    add_system("gbc", "Game Boy Color", {{"gambatte", "Gambatte"}, {"sameboy", "SameBoy"}});
    add_system("gb-gbc", "Game Boy / Game Boy Color", {{"gambatte", "Gambatte"}, {"sameboy", "SameBoy"}});
    add_system("gba", "Game Boy Advance", {{"mgba", "mGBA"}, {"vba_m", "VBA-M"}});
    add_system("nds", "Nintendo DS", {{"melonds", "melonDS"}, {"desmume", "DeSmuME"}});
    add_system("n64", "Nintendo 64", {{"mupen64plus_next", "Mupen64Plus-Next"}, {"parallel_n64", "ParaLLEl N64"}});
    add_system("ps1", "PlayStation", {{"pcsx_rearmed", "PCSX ReARMed"}, {"swanstation", "SwanStation"}, {"mednafen_psx_hw", "Beetle PSX HW"}, {"mednafen_psx", "Beetle PSX"}, {"beetle_psx_hw", "Beetle PSX HW"}, {"beetle_psx", "Beetle PSX"}});
    add_system("ps2", "PlayStation 2", {{"lrps2", "LRPS2"}, {"pcsx2", "PCSX2"}});
    add_system("psp", "PSP", {{"ppsspp", "PPSSPP"}});
    add_system("gamecube", "GameCube", {{"dolphin", "Dolphin"}});
    add_system("wii", "Wii", {{"dolphin", "Dolphin"}});
    add_system("3ds", "Nintendo 3DS", {{"citra", "Citra"}});
    add_system("switch", "Nintendo Switch", {{"yuzu", "Yuzu"}, {"ryujinx", "Ryujinx"}, {"suyu", "Suyu"}});
    add_system("snes", "SNES / Super Famicom", {{"snes9x", "Snes9x"}, {"bsnes", "bsnes"}, {"bsnes_mercury_balanced", "bsnes-mercury Balanced"}, {"mesen_s", "Mesen-S"}, {"snes9x_2010", "Snes9x 2010"}, {"snes9x_2005", "Snes9x 2005"}});
    add_system("nes", "NES / Famicom", {{"nestopia", "Nestopia"}, {"fceumm", "FCEUmm"}, {"mesen", "Mesen"}, {"quicknes", "QuickNES"}});
    add_system("pce", "PC Engine / TurboGrafx-16", {{"mednafen_pce_fast", "Beetle PCE FAST"}});
    add_system("sega-8-16", "Sega 8/16-bit", {{"genesis_plus_gx", "Genesis Plus GX"}, {"picodrive", "PicoDrive"}});

    add_extension_systems({"gb"}, "gb");
    add_extension_systems({"gbc"}, "gbc");
    add_extension_systems({"gba"}, "gba");
    add_extension_systems({"nds", "dsi"}, "nds");
    add_extension_systems({"n64", "z64", "v64"}, "n64");
    add_extension_systems({"cue", "pbp", "m3u"}, "ps1");
    add_extension_systems({"cso"}, "psp");
    add_extension_systems({"gcm", "gcz", "rvz"}, "gamecube");
    add_extension_systems({"wbfs", "wad"}, "wii");
    add_extension_systems({"xci", "nsp", "nsz"}, "switch");
    add_extension_systems({"3ds", "cia", "cci", "cxi"}, "3ds");
    add_extension_systems({"sfc", "smc"}, "snes");
    add_extension_systems({"nes", "fds"}, "nes");
    add_extension_systems({"pce"}, "pce");
    add_extension_systems({"gen", "smd", "sms", "gg", "sg"}, "sega-8-16");
}

std::vector<std::filesystem::path> LibretroCoreRegistry::default_core_dirs() {
    std::vector<std::filesystem::path> dirs;
    dirs.emplace_back("/srv/retroarch/cores");

    if (const char* home = std::getenv("HOME"); home != nullptr) {
        const std::filesystem::path home_path(home);
        dirs.push_back(home_path / ".config/retroarch/cores");
        dirs.push_back(home_path / ".var/app/org.libretro.RetroArch/config/retroarch/cores");
    }

    dirs.emplace_back("/usr/lib/x86_64-linux-gnu/libretro");
    dirs.emplace_back("/usr/lib/libretro");
    dirs.emplace_back("/usr/local/lib/libretro");
    dirs.emplace_back("/app/lib/libretro");

    return dirs;
}

std::optional<CoreChoice> LibretroCoreRegistry::system_core(std::string system_key) const {
    const auto it = systems_.find(normalize_token(std::move(system_key)));
    if (it == systems_.end()) {
        return std::nullopt;
    }

    for (const auto& candidate : it->second.cores) {
        if (auto core_path = find_core_file(candidate.key); core_path.has_value()) {
            return CoreChoice{
                it->second.display_name,
                candidate.display_name,
                *core_path,
            };
        }
    }

    return std::nullopt;
}

std::optional<CoreChoice> LibretroCoreRegistry::find_for_content(const std::filesystem::path& content_path) const {
    const auto extension = normalize_extension(content_path.extension().string());
    const auto it = extension_to_system_.find(extension);
    if (it == extension_to_system_.end()) {
        return std::nullopt;
    }

    return system_core(it->second);
}

void LibretroCoreRegistry::add_system(
    std::string key,
    std::string display_name,
    std::initializer_list<CoreCandidate> cores) {
    systems_[normalize_token(std::move(key))] = SystemEntry{
        std::move(display_name),
        std::vector<CoreCandidate>(cores),
    };
}

void LibretroCoreRegistry::add_extension_systems(
    std::initializer_list<std::string_view> extensions,
    std::string system_key) {
    for (const auto extension : extensions) {
        extension_to_system_[normalize_extension(std::string(extension))] = system_key;
    }
}

std::optional<std::filesystem::path> LibretroCoreRegistry::find_core_file(const std::string& core_key) const {
    const auto normalized = normalize_token(core_key);
    const std::vector<std::string> candidates{
        normalized + "_libretro.so",
        "lib" + normalized + "_libretro.so",
        normalized + ".so",
    };

    for (const auto& dir : core_dirs_) {
        if (!std::filesystem::exists(dir)) {
            continue;
        }

        for (const auto& candidate : candidates) {
            const auto path = dir / candidate;
            if (std::filesystem::exists(path)) {
                return path;
            }
        }
    }

    return std::nullopt;
}

std::string LibretroCoreRegistry::normalize_extension(std::string extension) {
    if (!extension.empty() && extension.front() == '.') {
        extension.erase(extension.begin());
    }

    return normalize_token(std::move(extension));
}

std::string LibretroCoreRegistry::normalize_token(std::string token) {
    std::transform(token.begin(), token.end(), token.begin(), [](unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });
    return token;
}

} // namespace archstreamer
