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

bool is_gb_family(std::string_view system_key) {
    return system_key == "gb" || system_key == "gbc" || system_key == "gb-gbc";
}

#if defined(ARCHSTREAMER_DEBUG_GB_LINK)

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
    const auto dir = std::filesystem::path(home) / ".config/retroarch/config" / "DoubleCherryGB";
    return dir / "DoubleCherryGB.opt";
}

#endif // ARCHSTREAMER_DEBUG_GB_LINK

} // namespace

bool LinkCableBackend::write_dual_gb_core_options() {
#if defined(ARCHSTREAMER_DEBUG_GB_LINK)
    const auto path = doublecherry_opt_path();
    if (path.empty()) {
        return false;
    }
    upsert_core_opt_file(path, {
        {"dcgb_emulated_gameboys", "2"},
        {"dcgb_gblink_enable", "enabled"},
        {"dcgb_number_of_local_screens", "2"},
        {"dcgb_single_screen_mp", "all players"},
        {"dcgb_multiplayer_linked_devive", "Auto"},
    });
    return std::filesystem::is_regular_file(path);
#else
    return false;
#endif
}

bool LinkCableBackend::write_single_gb_core_options() {
#if defined(ARCHSTREAMER_DEBUG_GB_LINK)
    const auto path = doublecherry_opt_path();
    if (path.empty()) {
        return false;
    }
    upsert_core_opt_file(path, {
        {"dcgb_emulated_gameboys", "1"},
        {"dcgb_number_of_local_screens", "1"},
    });
    return std::filesystem::is_regular_file(path);
#else
    return false;
#endif
}

std::optional<std::filesystem::path> LinkCableBackend::resolve_link_core(std::string_view system_key) {
#if defined(ARCHSTREAMER_DEBUG_GB_LINK)
    if (is_gb_family(system_key)) {
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
#else
    (void)system_key;
#endif
    return std::nullopt;
}

LinkCableBackend::StartResult LinkCableBackend::begin(
    std::string_view system_key,
    ClientId logical_host_client_id,
    ClientId logical_client_client_id,
    std::string logical_host_username,
    std::string logical_client_username,
    std::uint8_t seated_players,
    bool peers_already_running) {
    StartResult result;
    clear();

    if (!system_supports_link(system_key)) {
        result.message = "This system does not support ArchStreamer link yet";
        return result;
    }
    if (seated_players < 2) {
        result.message =
            "Link needs at least 2 seated players "
            "(each linked player will own one emulator instance)";
        return result;
    }

    client_a_ = logical_host_client_id;
    client_b_ = logical_client_client_id;
    user_a_ = std::move(logical_host_username);
    user_b_ = std::move(logical_client_username);
    result.logical_host_client_id = client_a_;
    result.logical_client_client_id = client_b_;
    result.logical_host_username = user_a_;
    result.logical_client_username = user_b_;

#if defined(ARCHSTREAMER_DEBUG_GB_LINK)
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
            "(DEBUG_GB_LINK: shared stream; per-user saves later.)";
        return result;
    }
#else
    if (is_gb_family(system_key)) {
        result.message =
            "GB/GBC dual-GB link is experimental; rebuild with "
            "-DARCHSTREAMER_DEBUG_GB_LINK=ON to enable it";
        return result;
    }
#endif

    // Multi-instance path: two SP slots already running, or promote one shared session.
    if (system_key == "gba" || system_key == "nds" || system_key == "switch") {
        mode_ = LinkCableMode::GbaNetpacket;
        result.ok = true;
        result.mode = mode_;
        if (peers_already_running) {
            result.needs_runtime_promotion = false;
            result.message =
                "Matched " + user_a_ + " ↔ " + user_b_ + " on " + std::string(system_key) +
                ". Both singleplayer instances are running (logical host=" + user_a_ +
                "). Cable/netpacket between them is not wired yet.";
        } else {
            result.needs_runtime_promotion = true;
            result.message =
                "Matched " + user_a_ + " ↔ " + user_b_ + " on " + std::string(system_key) +
                ". Promoting to Link runtime (logical host=" + user_a_ +
                "). Peer emulator + dual streams not started yet.";
        }
        return result;
    }

    result.message = "No link backend for this system yet";
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
#if defined(ARCHSTREAMER_DEBUG_GB_LINK)
    if (!relaunch_requested_) {
        return false;
    }
    relaunch_requested_ = false;
    return true;
#else
    relaunch_requested_ = false;
    return false;
#endif
}

std::optional<std::filesystem::path> LinkCableBackend::pending_core_path() const {
    if (pending_core_path_.empty()) {
        return std::nullopt;
    }
    return pending_core_path_;
}

} // namespace archstreamer
