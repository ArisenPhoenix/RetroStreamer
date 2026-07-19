#include "host/game_catalog_scanner.hpp"

#include <iostream>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: game_catalog_probe <content-root> [metadata-root]\n";
        return 2;
    }

    const auto catalog = archstreamer::scan_game_catalog(
        argv[1],
        archstreamer::LibretroCoreRegistry::ubuntu_defaults(),
        argc > 2 ? argv[2] : "");
    const auto list = catalog.list();

    std::cout << "Found " << list.games.size() << " supported games.\n";
    for (const auto& game : list.games) {
        const auto launch = catalog.launch_config_for(game.id);
        std::cout
            << game.display_name
            << " | " << game.system_name
            << " [" << game.system_key << ']'
            << " | " << game.core_name
            << " | " << game.language
            << '/' << game.region
            << '/' << game.version
            << " | players " << static_cast<int>(game.min_players)
            << '-' << static_cast<int>(game.max_players)
            << " | modes"
            << (game.supports_singleplayer ? " single" : "")
            << (game.supports_multiplayer ? " multi" : "")
            << "\n  id=" << game.id
            << "\n  identity=" << game.identity_key
            << "\n  asset_key=" << game.asset_key
            << "\n  core=" << launch.core_path
            << "\n  content=" << launch.content_path
            << '\n';
    }
}
