#pragma once

#include "host/console/game_catalog.hpp"
#include "host/console/retroarch_process.hpp"
#include "host/console/retroarch_resolve.hpp"
#include "host/console/save_profile.hpp"
#include "host/virtual/pad_settings.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace archstreamer {

struct SessionContentInfo {
    std::string active_display_name;
    std::filesystem::path catalog_content_path;
    std::string m3m_title_id;
    std::vector<std::string> playlist_discs;
    std::string system_key;
};

struct SessionLaunchAssets {
    SaveProfile save_profile;
    RetroArchLaunchConfig launch_config;
    ResolvedRetroArch resolved_retroarch;
    SessionContentInfo content;
};

struct SwitchLaunchContent {
    std::string content_stem;
    std::string title_id;
    /** True when this catalog title has a .m3m (version/policy map). */
    bool uses_m3m_map = false;
};

SessionContentInfo resolve_session_content_info(
    GameCatalog& catalog,
    const GameId& game_id);

SessionLaunchAssets prepare_session_launch_assets(
    const std::filesystem::path& save_root,
    PadSettings& pads,
    bool verbose,
    GameCatalog& catalog,
    const GameId& game_id,
    const std::string& save_username,
    std::string_view log_prefix = {});

void apply_session_content_launch_adjustments(
    PadSettings& pads,
    bool verbose,
    RetroArchLaunchConfig& launch_config,
    const SaveProfile& save_profile,
    const ResolvedRetroArch& resolved_retroarch,
    const SessionContentInfo& content,
    std::string_view log_prefix = {});

SwitchLaunchContent resolve_switch_launch_content(
    const SaveProfile& save_profile,
    const RetroArchLaunchConfig& launch_config,
    const SessionContentInfo& content);

} // namespace archstreamer
