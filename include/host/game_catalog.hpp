#pragma once

#include "common/protocol.hpp"
#include "host/retroarch_process.hpp"

#include <filesystem>
#include <functional>
#include <optional>
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
    void set_games(std::vector<HostedGame> games);
    void add_game(HostedGame game);
    GameList list() const;
    GameList delta_since(std::uint64_t client_catalog_revision) const;
    std::optional<GameInfo> find(const GameId& game_id) const;
    std::optional<std::reference_wrapper<const HostedGame>> find_hosted(const GameId& game_id) const;
    bool contains(const GameId& game_id) const;

    RetroArchLaunchConfig launch_config_for(
        const GameId& game_id,
        std::filesystem::path retroarch_path = "/usr/bin/retroarch") const;

private:
    std::vector<HostedGame> games_;
};

} // namespace archstreamer
