#include "host/game_catalog.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace archstreamer {

void GameCatalog::set_games(std::vector<HostedGame> games) {
    games_ = std::move(games);
}

void GameCatalog::add_game(HostedGame game) {
    games_.push_back(std::move(game));
}

GameList GameCatalog::list() const {
    GameList result;
    result.games.reserve(games_.size());
    for (const auto& game : games_) {
        result.games.push_back(game.info);
        result.catalog_revision = std::max(result.catalog_revision, game.info.updated_at);
    }

    return result;
}

GameList GameCatalog::delta_since(std::uint64_t client_catalog_revision) const {
    auto result = GameList{};
    result.catalog_revision = list().catalog_revision;
    result.full = client_catalog_revision == 0;
    result.games.reserve(games_.size());

    for (const auto& game : games_) {
        if (result.full || game.info.updated_at > client_catalog_revision) {
            result.games.push_back(game.info);
        }
    }

    return result;
}

std::optional<GameInfo> GameCatalog::find(const GameId& game_id) const {
    const auto game = find_hosted(game_id);
    if (!game.has_value()) {
        return std::nullopt;
    }

    return game->get().info;
}

std::optional<std::reference_wrapper<const HostedGame>> GameCatalog::find_hosted(const GameId& game_id) const {
    const auto it = std::find_if(games_.begin(), games_.end(), [&](const HostedGame& game) {
        return game.info.id == game_id;
    });

    if (it == games_.end()) {
        return std::nullopt;
    }

    return std::cref(*it);
}

bool GameCatalog::contains(const GameId& game_id) const {
    return find_hosted(game_id).has_value();
}

RetroArchLaunchConfig GameCatalog::launch_config_for(
    const GameId& game_id,
    std::filesystem::path retroarch_path) const {
    const auto game = find_hosted(game_id);
    if (!game.has_value()) {
        throw std::runtime_error("unknown game id");
    }

    return RetroArchLaunchConfig{
        std::move(retroarch_path),
        game->get().core_path,
        game->get().content_path,
        game->get().retroarch_args,
    };
}

} // namespace archstreamer
