#pragma once

#include "common/protocol.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace archstreamer {

enum class GameFilterMode {
    Any,
    SinglePlayer,
    Multiplayer,
};

struct GameFilter {
    GameFilterMode mode = GameFilterMode::Any;
    std::uint8_t requested_players = 1;
    std::optional<std::string> system_name;
    std::optional<std::string> language;
};

std::string normalize_filter_text(std::string_view value);
std::string acronym_for_filter_text(std::string_view value);
bool game_matches_filter(const GameInfo& game, const GameFilter& filter);
std::vector<std::string> languages_for_games(const GameList& source);
GameList filter_games(const GameList& source, const GameFilter& filter);
std::vector<std::string> systems_for_games(const GameList& source);
GameSessionMode default_session_mode_for_filter(GameFilterMode mode);

} // namespace archstreamer
