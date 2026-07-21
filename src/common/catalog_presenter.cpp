#include "common/catalog_presenter.hpp"

#include <ostream>

namespace archstreamer {

std::string format_game_summary(const GameInfo& game) {
    std::string summary = game.display_name + " | " + game.system_name;
    summary
        += " | " + game.language + '/' + game.region + '/' + game.version
        + " | players " + std::to_string(static_cast<int>(game.min_players))
        + '-' + std::to_string(static_cast<int>(game.max_players));
    if (game.supports_singleplayer) {
        summary += " | single";
    }
    if (game.supports_multiplayer) {
        summary += " multi";
    }
    return summary;
}

void print_game_catalog(std::ostream& out, const GameList& list) {
    for (std::size_t i = 0; i < list.games.size(); ++i) {
        const auto& game = list.games[i];
        out
            << i << ": " << game.display_name
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
            << "\n   id=" << game.id
            << "\n   asset_key=" << game.asset_key
            << '\n';
    }
}

const GameInfo* find_game_by_id(const GameList& list, const GameId& game_id) {
    for (const auto& game : list.games) {
        if (game.id == game_id) {
            return &game;
        }
    }
    return nullptr;
}

} // namespace archstreamer
