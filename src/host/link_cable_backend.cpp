#include "host/link_cable_backend.hpp"

#include "common/link_capability.hpp"
#include "common/platform/paths.hpp"
#include "host/libretro_core_registry.hpp"

#include <fstream>
#include <unordered_map>
#include <utility>
#include <vector>

namespace archstreamer {
namespace {

void upsert_core_opt_file(
    const std::filesystem::path& path,
    const std::vector<std::pair<std::string, std::string>>& options) {
    if (options.empty()) {
        return;
    }
    std::filesystem::create_directories(path.parent_path());

    std::unordered_map<std::string, std::string> values;
    std::vector<std::string> order;
    if (std::ifstream in(path); in) {
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty() || line[0] == '#') {
                continue;
            }
            const auto eq = line.find('=');
            if (eq == std::string::npos) {
                continue;
            }
            auto key = line.substr(0, eq);
            while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) {
                key.pop_back();
            }
            if (values.emplace(key, line).second) {
                order.push_back(key);
            } else {
                values[key] = line;
            }
        }
    }

    for (const auto& [key, value] : options) {
        const auto line = key + " = \"" + value + "\"";
        if (values.emplace(key, line).second) {
            order.push_back(key);
        } else {
            values[key] = line;
        }
    }

    std::ofstream out(path, std::ios::trunc);
    if (!out) {
        return;
    }
    for (const auto& key : order) {
        out << values[key] << '\n';
    }
}

std::filesystem::path doublecherry_opt_path() {
    const auto home = user_home_directory();
    if (home.empty()) {
        return {};
    }
    // RetroArch looks up by core display folder; nightly uses DoubleCherryGB.
    const auto dir = std::filesystem::path(home) / ".config/retroarch/config" / "DoubleCherryGB";
    return dir / "DoubleCherryGB.opt";
}

bool is_gb_family(std::string_view system_key) {
    return system_key == "gb" || system_key == "gbc" || system_key == "gb-gbc";
}

} // namespace

bool LinkCableBackend::write_dual_gb_core_options() {
    const auto path = doublecherry_opt_path();
    if (path.empty()) {
        return false;
    }
    // README: emulated gameboys >= 2 enables local multi-GB + internal link cable.
    // single_screen_mp left off so both screens stay visible on the shared stream.
    upsert_core_opt_file(path, {
        {"dcgb_emulated_gameboys", "2"},
        {"dcgb_gblink_enable", "enabled"},
        {"dcgb_number_of_local_screens", "2"},
        {"dcgb_single_screen_mp", "all players"},
        {"dcgb_multiplayer_linked_devive", "Auto"},
    });
    return std::filesystem::is_regular_file(path);
}

bool LinkCableBackend::write_single_gb_core_options() {
    const auto path = doublecherry_opt_path();
    if (path.empty()) {
        return false;
    }
    upsert_core_opt_file(path, {
        {"dcgb_emulated_gameboys", "1"},
        {"dcgb_number_of_local_screens", "1"},
    });
    return std::filesystem::is_regular_file(path);
}

std::optional<std::filesystem::path> LinkCableBackend::resolve_link_core(std::string_view system_key) {
    const auto registry = LibretroCoreRegistry::defaults();
    if (is_gb_family(system_key)) {
        if (const auto core = registry.system_core("gb"); core.has_value()) {
            const auto name = core->core_path.filename().string();
            // Prefer DoubleCherryGB when the registry already picked it (listed first).
            if (name.find("DoubleCherry") != std::string::npos ||
                name.find("doublecherry") != std::string::npos) {
                return core->core_path;
            }
        }
        // Direct lookup even if gambatte is still preferred for catalog listing.
        for (const auto& dir : LibretroCoreRegistry::default_core_dirs()) {
            for (const char* name : {
                     "doublecherrygb_libretro.so",
                     "DoubleCherryGB_libretro.so",
                     "tgbdual_libretro.so",
                 }) {
                const auto path = dir / name;
                if (std::filesystem::is_regular_file(path)) {
                    return path;
                }
            }
        }
        return std::nullopt;
    }
    if (system_key == "gba") {
        for (const auto& dir : LibretroCoreRegistry::default_core_dirs()) {
            const auto path = dir / "gpsp_libretro.so";
            if (std::filesystem::is_regular_file(path)) {
                return path;
            }
        }
        return std::nullopt;
    }
    return std::nullopt;
}

LinkCableBackend::StartResult LinkCableBackend::begin(
    std::string_view system_key,
    ClientId client_a,
    ClientId client_b,
    std::string user_a,
    std::string user_b,
    std::uint8_t seated_players) {
    StartResult result;
    clear();

    if (!system_supports_link(system_key)) {
        result.message = "This system does not support ArchStreamer link yet";
        return result;
    }
    if (seated_players < 2) {
        result.message =
            "Virtual cable needs at least 2 seated players in the session "
            "(each linked player controls one emulated handheld)";
        return result;
    }

    client_a_ = client_a;
    client_b_ = client_b;
    user_a_ = std::move(user_a);
    user_b_ = std::move(user_b);

    if (is_gb_family(system_key)) {
        const auto core = resolve_link_core(system_key);
        if (!core.has_value()) {
            result.message =
                "DoubleCherryGB core not found. Install it under ~/.config/retroarch/cores "
                "(doublecherrygb_libretro.so) for Gen1/Gen2 virtual cable.";
            return result;
        }
        if (!write_dual_gb_core_options()) {
            result.message = "Failed to write DoubleCherryGB dual-GB core options";
            return result;
        }
        mode_ = LinkCableMode::LocalDualGb;
        pending_core_path_ = *core;
        relaunch_requested_ = true;
        result.ok = true;
        result.needs_relaunch = true;
        result.mode = mode_;
        result.core_path = *core;
        result.message =
            "Virtual cable ready for " + user_a_ + " ↔ " + user_b_ +
            ". Reloading with 2 emulated Game Boys — each pad controls one machine. "
            "Visit the Cable Club / link room in-game. "
            "(Both screens share one stream; per-user save isolation comes later.)";
        return result;
    }

    if (system_key == "gba") {
        const auto core = resolve_link_core(system_key);
        if (!core.has_value()) {
            result.message =
                "gpSP core not found. Install gpsp_libretro.so for GBA wireless/link support.";
            return result;
        }
        mode_ = LinkCableMode::GbaNetpacket;
        pending_core_path_ = *core;
        // Dual-instance + dual media not wired yet — surface a clear next-step message.
        result.ok = false;
        result.mode = mode_;
        result.core_path = *core;
        result.message =
            "Matched " + user_a_ + " ↔ " + user_b_ +
            ", but GBA wireless (gpSP netpacket) needs dual RetroArch instances "
            "and dual streams — not started yet. GB/GBC local cable is available now.";
        mode_ = LinkCableMode::None;
        pending_core_path_.clear();
        return result;
    }

    result.message = "No link-cable backend for this system yet";
    return result;
}

void LinkCableBackend::clear() {
    mode_ = LinkCableMode::None;
    relaunch_requested_ = false;
    pending_core_path_.clear();
    client_a_ = 0;
    client_b_ = 0;
    user_a_.clear();
    user_b_.clear();
}

bool LinkCableBackend::consume_relaunch_request() {
    if (!relaunch_requested_) {
        return false;
    }
    relaunch_requested_ = false;
    return true;
}

std::optional<std::filesystem::path> LinkCableBackend::pending_core_path() const {
    if (pending_core_path_.empty()) {
        return std::nullopt;
    }
    return pending_core_path_;
}

} // namespace archstreamer
