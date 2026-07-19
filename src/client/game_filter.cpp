#include "client/game_filter.hpp"

#include <algorithm>
#include <cctype>

namespace archstreamer {

std::string normalize_filter_text(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (const unsigned char character : value) {
        result.push_back(static_cast<char>(std::tolower(character)));
    }
    return result;
}

std::string acronym_for_filter_text(std::string_view value) {
    std::string result;
    bool at_word_start = true;
    for (const unsigned char character : value) {
        if (std::isalnum(character)) {
            if (at_word_start) {
                result.push_back(static_cast<char>(std::tolower(character)));
            }
            at_word_start = false;
        } else {
            at_word_start = true;
        }
    }
    return result;
}

bool game_matches_filter(const GameInfo& game, const GameFilter& filter) {
    if (filter.mode == GameFilterMode::SinglePlayer && !game.supports_singleplayer) {
        return false;
    }
    if (filter.mode == GameFilterMode::Multiplayer) {
        if (!game.supports_multiplayer) {
            return false;
        }
        if (filter.requested_players > game.max_players) {
            return false;
        }
    }
    if (filter.system_name.has_value() && !filter.system_name->empty()) {
        const auto wanted = normalize_filter_text(*filter.system_name);
        const auto system = normalize_filter_text(game.system_name);
        const auto system_key = normalize_filter_text(game.system_key);
        const auto acronym = acronym_for_filter_text(game.system_name);
        if (system.find(wanted) == std::string::npos && system_key != wanted && acronym != wanted) {
            return false;
        }
    }
    if (filter.language.has_value() && !filter.language->empty()) {
        const auto wanted = normalize_filter_text(*filter.language);
        const auto language = normalize_filter_text(game.language);
        if (language != wanted) {
            return false;
        }
    }

    return true;
}

std::vector<std::string> languages_for_games(const GameList& source) {
    std::vector<std::string> languages;
    for (const auto& game : source.games) {
        if (game.language.empty()) {
            continue;
        }
        if (std::find(languages.begin(), languages.end(), game.language) == languages.end()) {
            languages.push_back(game.language);
        }
    }
    std::sort(languages.begin(), languages.end());
    return languages;
}

GameList filter_games(const GameList& source, const GameFilter& filter) {
    GameList result;
    result.games.reserve(source.games.size());
    for (const auto& game : source.games) {
        if (game_matches_filter(game, filter)) {
            result.games.push_back(game);
        }
    }
    return result;
}

std::vector<std::string> systems_for_games(const GameList& source) {
    std::vector<std::string> systems;
    for (const auto& game : source.games) {
        if (game.system_name.empty()) {
            continue;
        }
        if (std::find(systems.begin(), systems.end(), game.system_name) == systems.end()) {
            systems.push_back(game.system_name);
        }
    }
    std::sort(systems.begin(), systems.end());
    return systems;
}

GameSessionMode default_session_mode_for_filter(GameFilterMode mode) {
    if (mode == GameFilterMode::Multiplayer) {
        return GameSessionMode::Multiplayer;
    }

    return GameSessionMode::SinglePlayer;
}

} // namespace archstreamer
