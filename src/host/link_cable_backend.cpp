#include "host/link_cable_backend.hpp"

#include "common/link_capability.hpp"
#include "common/platform/paths.hpp"
#include "host/libretro_core_registry.hpp"
#include "host/nds/melonds_backend.hpp"
#include "host/standalone_emulator.hpp"

#include <fstream>
#include <unordered_map>
#include <utility>
#include <vector>

namespace archstreamer {
namespace {

constexpr std::uint16_t kDefaultGbaNetplayPort = 55435;

bool is_gb_family(std::string_view system_key) {
    return system_key == "gb" || system_key == "gbc" || system_key == "gb-gbc";
}

bool is_netplay_flag(const std::string& arg) {
    return arg == "-H" || arg == "--host" || arg == "-C" || arg == "--connect" ||
        arg.rfind("--connect=", 0) == 0 || arg == "--port" || arg.rfind("--port=", 0) == 0 ||
        arg == "--nick" || arg.rfind("--nick=", 0) == 0;
}

bool netplay_flag_takes_value(const std::string& arg) {
    return arg == "-C" || arg == "--connect" || arg == "--port" || arg == "--nick";
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

std::optional<std::filesystem::path> find_gpsp_core() {
    for (const auto& dir : LibretroCoreRegistry::default_core_dirs()) {
        for (const char* name : {"gpsp_libretro.so", "gpsp_libretro.dll"}) {
            const auto path = dir / name;
            if (std::filesystem::is_regular_file(path)) {
                return path;
            }
        }
    }
    return std::nullopt;
}

} // namespace

void LinkCableBackend::apply_netplay_launch_args(
    std::vector<std::string>& extra_args,
    bool is_host,
    std::uint16_t port,
    const std::string& nick) {
    std::vector<std::string> kept;
    kept.reserve(extra_args.size());
    for (std::size_t i = 0; i < extra_args.size();) {
        const auto& arg = extra_args[i];
        if (is_netplay_flag(arg)) {
            if (netplay_flag_takes_value(arg) && arg.find('=') == std::string::npos &&
                i + 1 < extra_args.size()) {
                i += 2;
            } else {
                ++i;
            }
            continue;
        }
        kept.push_back(arg);
        ++i;
    }
    extra_args = std::move(kept);

    if (is_host) {
        extra_args.push_back("--host");
        extra_args.push_back("--port");
        extra_args.push_back(std::to_string(port));
    } else {
        // Preferred form: address|port (see RetroArch --connect docs).
        extra_args.push_back("--connect");
        extra_args.push_back("127.0.0.1|" + std::to_string(port));
    }
    if (!nick.empty()) {
        extra_args.push_back("--nick");
        extra_args.push_back(nick);
    }
}

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
#endif
    if (system_key == "gba") {
        return find_gpsp_core();
    }
    if (system_key == "nds") {
        for (const auto& dir : LibretroCoreRegistry::default_core_dirs()) {
            for (const char* name : {
                     "melondsds_libretro.so",
                     "melonds_libretro.so",
                     "melondsds_libretro.dll",
                     "melonds_libretro.dll",
                 }) {
                const auto path = dir / name;
                if (std::filesystem::is_regular_file(path)) {
                    return path;
                }
            }
        }
        return std::nullopt;
    }
    return std::nullopt;
}

LinkCableBackend::StartResult LinkCableBackend::begin(
    std::string_view system_key,
    ClientId logical_host_client_id,
    ClientId logical_client_client_id,
    std::string logical_host_username,
    std::string logical_client_username,
    std::uint8_t seated_players,
    bool peers_already_running,
    std::uint16_t netplay_port_hint) {
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
    netplay_port_ = netplay_port_hint == 0 ? kDefaultGbaNetplayPort : netplay_port_hint;
    result.netplay_port = netplay_port_;

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

    if (system_key == "gba") {
        const auto core = resolve_link_core("gba");
        if (!core.has_value()) {
            result.message =
                "gpSP core not found. Install gpsp_libretro under ~/.config/retroarch/cores "
                "(GBA link uses RetroArch netpacket, which mGBA does not provide).";
            return result;
        }
        mode_ = LinkCableMode::GbaNetpacket;
        pending_core_path_ = *core;
        result.ok = true;
        result.mode = mode_;
        result.core_path = *core;
        if (peers_already_running) {
            result.needs_gba_netplay = true;
            result.needs_runtime_promotion = false;
            result.message =
                "Matched " + user_a_ + " ↔ " + user_b_ +
                ". Starting gpSP netplay cable on 127.0.0.1:" + std::to_string(netplay_port_) +
                " (host=" + user_a_ + "). Enter the Cable Club / link room in-game.";
        } else {
            result.needs_runtime_promotion = true;
            result.message =
                "Matched " + user_a_ + " ↔ " + user_b_ +
                ". Same-slot GBA link still needs a second emulator instance "
                "(use two concurrent singleplayer sessions, then Link).";
        }
        return result;
    }

    if (system_key == "nds") {
        if (melonds_runtime_available() && peers_already_running) {
            mode_ = LinkCableMode::NdsMelonLan;
            result.ok = true;
            result.mode = mode_;
            result.needs_nds_lan = true;
            result.message =
                "Matched " + user_a_ + " ↔ " + user_b_ +
                ". melonDS LAN armed — open Local Wireless in-game. "
                "(standalone melonDS; no relaunch)";
            return result;
        }

        const auto core = resolve_link_core("nds");
        if (!core.has_value()) {
            result.message =
                "NDS Link needs standalone melonDS under /srv/emus/melonDS "
                "(or ARCHSTREAMER_MELONDS), or a melonds_libretro core for the "
                "RetroArch netplay fallback.";
            return result;
        }
        mode_ = LinkCableMode::GbaNetpacket;
        pending_core_path_ = *core;
        result.ok = true;
        result.mode = mode_;
        result.core_path = *core;
        if (peers_already_running) {
            result.needs_nds_netplay = true;
            result.message =
                "Matched " + user_a_ + " ↔ " + user_b_ +
                ". Starting melonDS (libretro) netplay cable on 127.0.0.1:" +
                std::to_string(netplay_port_) + " (host=" + user_a_ +
                "). Enter Local Wireless in-game.";
        } else {
            result.needs_runtime_promotion = true;
            result.message =
                "Matched " + user_a_ + " ↔ " + user_b_ +
                ". Same-slot NDS link still needs a second emulator instance "
                "(use two concurrent singleplayer sessions, then Link).";
        }
        return result;
    }

    if (system_key == "switch") {
        if (!ryujinx_runtime_available()) {
            result.message =
                "Switch Link needs Ryujinx with ldn_mitm. Install Ryujinx under " +
                default_ryujinx_runtime_root().string() +
                " (or set ARCHSTREAMER_RYUJINX), then relaunch both sessions.";
            return result;
        }
        mode_ = LinkCableMode::SwitchLdnMitm;
        result.ok = true;
        result.mode = mode_;
        // Sessions already start with multiplayer_mode=ldn_mitm — no relaunch.
        result.message =
            "Matched " + user_a_ + " ↔ " + user_b_ +
            ". Local Wireless (ldn_mitm) is enabled — open Local Play / LDN in-game. "
            "Both players must be on Ryujinx with the same title.";
        return result;
    }

    result.message = "No link backend for this system yet";
    return result;
}

void LinkCableBackend::clear() {
    mode_ = LinkCableMode::None;
    relaunch_requested_ = false;
    pending_core_path_.clear();
    netplay_port_ = kDefaultGbaNetplayPort;
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
