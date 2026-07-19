#include "common/protocol.hpp"

namespace archstreamer {

ClientRole role_for_player_count(std::uint8_t requested_players) {
    return requested_players == 0 ? ClientRole::Viewer : ClientRole::Player;
}

bool valid_player_count(std::uint8_t requested_players) {
    return requested_players <= MaxPlayersPerClient;
}

bool valid_controller_info_count(std::size_t count) {
    return count <= MaxPlayersPerClient;
}

bool valid_game_player_limits(std::uint8_t min_players, std::uint8_t max_players) {
    return min_players > 0 && max_players >= min_players && max_players <= MaxRetroArchPorts;
}

bool valid_username(std::string_view username) {
    if (username.empty() || username.size() > 64) {
        return false;
    }

    for (const char character : username) {
        const bool alpha = (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z');
        const bool digit = character >= '0' && character <= '9';
        const bool symbol = character == '_' || character == '-';
        if (!alpha && !digit && !symbol) {
            return false;
        }
    }

    return true;
}

} // namespace archstreamer
