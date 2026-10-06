#include "host/console/launch_content.hpp"

#include "host/console/save_profile.hpp"
#include "host/console/switch_save_share.hpp"

#include <iostream>
#include <utility>

namespace archstreamer {

SessionContentInfo resolve_session_content_info(
    GameCatalog& catalog,
    const GameId& game_id) {
    SessionContentInfo content;
    if (const auto hosted = catalog.find_hosted(game_id); hosted.has_value()) {
        auto hosted_game = hosted->get();
        content.system_key = hosted_game.info.system_key;
        content.active_display_name = hosted_game.info.display_name;
        content.catalog_content_path = hosted_game.content_path;
        content.m3m_title_id = hosted_game.m3m_title_id;
        content.m3m_texture_pack = hosted_game.m3m_texture_pack;
        content.playlist_discs = hosted_game.info.playlist_discs;
    } else if (const auto info = catalog.find(game_id); info.has_value()) {
        content.system_key = info->system_key;
        content.active_display_name = info->display_name;
        content.playlist_discs = info->playlist_discs;
    }
    return content;
}

SessionLaunchAssets prepare_session_launch_assets(
    const std::filesystem::path& save_root,
    PadSettings& pads,
    bool verbose,
    GameCatalog& catalog,
    const GameId& game_id,
    const std::string& save_username,
    std::string_view log_prefix) {
    auto assets = SessionLaunchAssets{};
    assets.save_profile = prepare_save_profile(save_root, save_username);
    assets.launch_config = catalog.launch_config_for(game_id);
    assets.resolved_retroarch = resolve_retroarch();
    assets.content = resolve_session_content_info(catalog, game_id);
    apply_session_content_launch_adjustments(
        pads,
        verbose,
        assets.launch_config,
        assets.save_profile,
        assets.resolved_retroarch,
        assets.content,
        log_prefix);
    return assets;
}

void apply_session_content_launch_adjustments(
    PadSettings& pads,
    bool verbose,
    RetroArchLaunchConfig& launch_config,
    const SaveProfile& save_profile,
    const ResolvedRetroArch& resolved_retroarch,
    const SessionContentInfo& content,
    std::string_view log_prefix) {
    if (!launch_config.standalone && content.system_key == "ps2") {
        std::cout
            << log_prefix
            << "PS2 memcards: " << user_ps2_memcard_directory(save_profile) << '\n';
    }

#if !defined(_WIN32)
    if (!launch_config.standalone &&
        (content.system_key == "ps1" || content.system_key == "ps2" ||
         content.system_key == "psp") &&
        pads.retroarch_joypad_driver == "sdl2") {
        if (log_prefix.empty()) {
            std::cout
                << "Note: forcing joypad driver udev for " << content.system_key
                << " (sdl2 stalls PlayStation cores).\n";
        } else {
            std::cout
                << log_prefix << "forcing joypad driver udev for " << content.system_key
                << " (sdl2 stalls PlayStation cores).\n";
        }
        pads.retroarch_joypad_driver = "udev";
    }
#endif

    if (!launch_config.standalone) {
        launch_config.retroarch_path = resolved_retroarch.display_path;
        launch_config.command_prefix = resolved_retroarch.argv_prefix;
    }
    if (verbose && !launch_config.standalone) {
        launch_config.extra_args.insert(launch_config.extra_args.begin(), "--verbose");
    }
}

SwitchLaunchContent resolve_switch_launch_content(
    const SaveProfile& save_profile,
    const RetroArchLaunchConfig& launch_config,
    const SessionContentInfo& content) {
    SwitchLaunchContent switch_content;
    switch_content.content_stem = !content.catalog_content_path.empty()
        ? content.catalog_content_path.stem().string()
        : launch_config.content_path.stem().string();
    const auto catalog_m3m = switch_m3m_map_for_title(content.catalog_content_path);
    const auto launch_m3m = switch_m3m_map_for_title(launch_config.content_path);
    switch_content.uses_m3m_map =
        !content.m3m_title_id.empty() || !catalog_m3m.empty() || !launch_m3m.empty();
    const auto& map_path = !catalog_m3m.empty() ? catalog_m3m : launch_m3m;

    if (switch_content.uses_m3m_map) {
        switch_content.title_id = content.m3m_title_id;
        if (switch_content.title_id.empty()) {
            switch_content.title_id = resolve_switch_title_id_for_catalog(
                save_profile,
                switch_content.content_stem,
                map_path,
                true);
        }
        std::cout
            << "switch save share: \"" << switch_content.content_stem
            << "\" using .m3m map"
            << (switch_content.title_id.empty() ? "" : " TITLE_ID=" + switch_content.title_id)
            << '\n';
    } else {
        switch_content.title_id = resolve_switch_title_id_for_catalog(
            save_profile,
            switch_content.content_stem,
            content.catalog_content_path.empty()
                ? launch_config.content_path
                : content.catalog_content_path,
            false);
        std::cout
            << "switch save share: \"" << switch_content.content_stem
            << "\" has no .m3m; catalog stem is identity"
            << (switch_content.title_id.empty()
                    ? ""
                    : " (cached title " + switch_content.title_id + ")")
            << '\n';
    }
    switch_content.texture_pack = content.m3m_texture_pack;
    return switch_content;
}

} // namespace archstreamer
