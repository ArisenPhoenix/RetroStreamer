#pragma once

#include "common/protocol.hpp"
#include "host/retroarch_process.hpp"

#include <algorithm>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace archstreamer {

struct HostedGame {
    GameInfo info;
    std::filesystem::path core_path;
    std::filesystem::path content_path;
    std::vector<std::string> retroarch_args;
};

class GameCatalog {
public:
    void set_games(std::vector<HostedGame> games) {
        games_ = std::move(games);
    }

    void add_game(HostedGame game) {
        games_.push_back(std::move(game));
    }

    GameList list() const {
        GameList result;
        result.games.reserve(games_.size());
        for (const auto& game : games_) {
            result.games.push_back(game.info);
            result.catalog_revision = std::max(result.catalog_revision, game.info.updated_at);
        }

        return result;
    }

    GameList delta_since(std::uint64_t client_catalog_revision) const {
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

    std::optional<GameInfo> find(const GameId& game_id) const {
        const auto game = find_hosted(game_id);
        if (!game.has_value()) {
            return std::nullopt;
        }

        return game->get().info;
    }

    std::optional<std::reference_wrapper<const HostedGame>> find_hosted(const GameId& game_id) const {
        const auto it = std::find_if(games_.begin(), games_.end(), [&](const HostedGame& game) {
            return game.info.id == game_id;
        });

        if (it == games_.end()) {
            return std::nullopt;
        }

        return std::cref(*it);
    }

    bool contains(const GameId& game_id) const {
        return find_hosted(game_id).has_value();
    }

    RetroArchLaunchConfig launch_config_for(
        const GameId& game_id,
        std::filesystem::path retroarch_path = "/usr/bin/retroarch") const {
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

private:
    std::vector<HostedGame> games_;
};

} // namespace archstreamer
