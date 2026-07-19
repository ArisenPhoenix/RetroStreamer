#include "client/session_client.hpp"

#include <stdexcept>
#include <utility>

namespace archstreamer {

ClientHello SessionClient::make_hello(
    std::string username,
    std::string display_name,
    std::optional<GameId> selected_game_id,
    GameSessionMode session_mode,
    std::uint8_t requested_players,
    std::vector<ControllerInfo> controllers,
    bool wants_video,
    bool wants_audio) const {
    if (!valid_player_count(requested_players)) {
        throw std::runtime_error("invalid requested player count");
    }
    if (!valid_username(username)) {
        throw std::runtime_error("username must be 1-64 characters and contain only letters, numbers, underscores, or hyphens");
    }
    if (display_name.empty()) {
        display_name = username;
    }
    if (!valid_controller_info_count(controllers.size())) {
        throw std::runtime_error("client can describe at most two controllers");
    }

    return ClientHello{
        std::move(username),
        std::move(display_name),
        std::move(selected_game_id),
        session_mode,
        requested_players,
        std::move(controllers),
        wants_video,
        wants_audio,
    };
}

ClientConfig SessionClient::make_config(
    std::optional<std::string> username,
    std::optional<std::string> display_name,
    std::optional<GameId> selected_game_id,
    GameSessionMode session_mode,
    std::uint8_t requested_players,
    std::vector<ControllerInfo> controllers,
    bool wants_video,
    bool wants_audio) const {
    if (!valid_player_count(requested_players)) {
        throw std::runtime_error("invalid requested player count");
    }
    if (username.has_value() && !valid_username(*username)) {
        throw std::runtime_error("username must be 1-64 characters and contain only letters, numbers, underscores, or hyphens");
    }
    if (!valid_controller_info_count(controllers.size())) {
        throw std::runtime_error("client can describe at most two controllers");
    }

    return ClientConfig{
        std::move(username),
        std::move(display_name),
        std::move(selected_game_id),
        session_mode,
        requested_players,
        std::move(controllers),
        wants_video,
        wants_audio,
    };
}

GameListRequest SessionClient::make_game_list_request(std::uint64_t client_catalog_revision) const {
    return GameListRequest{client_catalog_revision};
}

void SessionClient::apply_welcome(const HostWelcome& welcome) {
    client_id_ = welcome.client_id;
}

void SessionClient::apply_game_list(GameList game_list) {
    game_list_ = std::move(game_list);
}

void SessionClient::apply_seats(SeatAssignment seats) {
    seats_ = std::move(seats);
}

std::optional<ClientId> SessionClient::client_id() const {
    return client_id_;
}

const SeatAssignment& SessionClient::seats() const {
    return seats_;
}

const GameList& SessionClient::game_list() const {
    return game_list_;
}

} // namespace archstreamer
