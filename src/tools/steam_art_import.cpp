#include "common/catalog_paths.hpp"
#include "common/steam_art_import.hpp"
#include "host/game_catalog_scanner.hpp"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    auto content_root = std::filesystem::path{archstreamer::DefaultRomRoot};
    auto metadata_root = std::filesystem::path{archstreamer::DefaultMetaRoot};
    auto assets_root = std::filesystem::path{archstreamer::DefaultArtRoot};
    archstreamer::SteamArtImportOptions options;

    bool content_set = false;
    bool metadata_set = false;
    bool assets_set = false;

    for (int i = 1; i < argc; ++i) {
        const auto arg = std::string{argv[i]};
        if (arg == "--overwrite") {
            options.overwrite = true;
        } else if (arg == "--dry-run") {
            options.dry_run = true;
        } else if (arg == "--steam-config") {
            if (i + 1 >= argc) {
                std::cerr << "missing value for --steam-config\n";
                return 2;
            }
            options.steam_config_dir = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            std::cerr
                << "usage: steam_art_import [content-root] [metadata-root] [assets-root]\n"
                << "       [--steam-config DIR] [--overwrite] [--dry-run]\n"
                << "\n"
                << "Copies Steam grid art into Art/<asset_key>/{boxart,grid,hero,logo,icon}.png\n";
            return 0;
        } else if (!content_set) {
            content_root = arg;
            content_set = true;
        } else if (!metadata_set) {
            metadata_root = arg;
            metadata_set = true;
        } else if (!assets_set) {
            assets_root = arg;
            assets_set = true;
        } else {
            std::cerr << "unknown argument: " << arg << '\n';
            return 2;
        }
    }

    if (options.steam_config_dir.empty()) {
        const auto discovered = archstreamer::discover_steam_config_dir();
        if (!discovered.has_value()) {
            std::cerr << "could not discover Steam config dir (pass --steam-config)\n";
            return 1;
        }
        options.steam_config_dir = *discovered;
    }

    const auto catalog = archstreamer::scan_game_catalog(
        content_root,
        archstreamer::LibretroCoreRegistry::ubuntu_defaults(),
        metadata_root);
    const auto list = catalog.list();

    std::vector<archstreamer::GameArtImportTarget> targets;
    targets.reserve(list.games.size());
    for (const auto& game : list.games) {
        archstreamer::GameArtImportTarget target;
        target.asset_key = game.asset_key;
        target.display_name = game.display_name;
        target.canonical_name = game.canonical_name;
        if (const auto hosted = catalog.find_hosted(game.id); hosted.has_value()) {
            target.content_path = hosted->get().content_path;
        }
        targets.push_back(std::move(target));
    }

    std::cout
        << "Steam config: " << options.steam_config_dir
        << "\nAssets root:  " << assets_root
        << "\nCatalog games:" << targets.size()
        << (options.dry_run ? "\nMode:         dry-run" : "")
        << '\n';

    const auto result = archstreamer::import_steam_grid_art(targets, assets_root, options);
    std::cout
        << "shortcuts=" << result.shortcuts_read
        << " matched=" << result.matched_games
        << " copied=" << result.files_copied
        << " replaced=" << result.files_replaced
        << " skipped=" << result.files_skipped
        << " unmatched=" << result.unmatched_shortcuts.size()
        << '\n';

    if (!result.unmatched_shortcuts.empty()) {
        std::cout << "unmatched shortcuts:\n";
        const auto limit = std::min<std::size_t>(result.unmatched_shortcuts.size(), 20);
        for (std::size_t i = 0; i < limit; ++i) {
            std::cout << "  - " << result.unmatched_shortcuts[i] << '\n';
        }
        if (result.unmatched_shortcuts.size() > limit) {
            std::cout << "  ... and " << (result.unmatched_shortcuts.size() - limit) << " more\n";
        }
    }

    if (result.shortcuts_read == 0) {
        std::cerr << "no shortcuts found in " << (options.steam_config_dir / "shortcuts.vdf") << '\n';
        return 1;
    }
    return 0;
}
