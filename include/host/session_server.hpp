#pragma once

#include "host/game_catalog.hpp"
#include "host/input_router.hpp"
#include "host/seat_manager.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace archstreamer {

struct ConnectedClient {
    ClientId client_id = 0;
    std::string username;
    std::string display_name;
    ClientConfig config;
};

class SessionServer {
public:
    SessionServer(InputRouter& input_router, GameCatalog& game_catalog)
        : input_router_(input_router), game_catalog_(game_catalog) {}

    HostWelcome connect(ClientId client_id, const ClientHello& hello) {
        if (!valid_username(hello.username)) {
            throw std::runtime_error("client supplied an invalid username");
        }

        ClientConfig config;
        config.selected_game_id = hello.selected_game_id;
        config.session_mode = hello.session_mode;
        config.requested_players = hello.requested_players;
        config.controllers = hello.controllers;
        config.wants_video = hello.wants_video;
        config.wants_audio = hello.wants_audio;
        set_client_config(client_id, hello.username, hello.display_name, config);

        HostWelcome welcome;
        welcome.client_id = client_id;
        welcome.max_players_for_client = MaxPlayersPerClient;
        welcome.host_is_player = seat_manager_.host_player_count() > 0;
        return welcome;
    }

    void set_host_player_count(std::uint8_t player_count) {
        seat_manager_.set_host_player_count(player_count);
        rebuild_seats();
    }

    void set_client_config(ClientId client_id, const ClientConfig& config) {
        const auto existing = clients_.find(client_id);
        if (existing == clients_.end()) {
            throw std::runtime_error("client is not connected");
        }

        auto username = existing->second.username;
        auto display_name = existing->second.display_name;
        auto updated = config;
        if (updated.username.has_value()) {
            if (!valid_username(*updated.username)) {
                throw std::runtime_error("client supplied an invalid username");
            }
            username = *updated.username;
        }
        if (updated.display_name.has_value()) {
            display_name = *updated.display_name;
        }
        set_client_config(client_id, std::move(username), std::move(display_name), updated);
    }

    const ConnectedClient& client(ClientId client_id) const {
        const auto it = clients_.find(client_id);
        if (it == clients_.end()) {
            throw std::runtime_error("client is not connected");
        }

        return it->second;
    }

private:
    void set_client_config(
        ClientId client_id,
        std::string username,
        std::string display_name,
        const ClientConfig& config) {
        if (!valid_player_count(config.requested_players)) {
            throw std::runtime_error("client requested too many players");
        }
        if (!valid_controller_info_count(config.controllers.size())) {
            throw std::runtime_error("client supplied too many controllers");
        }
        if (config.selected_game_id.has_value() && !game_catalog_.contains(*config.selected_game_id)) {
            throw std::runtime_error("client selected an unknown game");
        }

        clients_[client_id] = ConnectedClient{
            client_id,
            std::move(username),
            std::move(display_name),
            config,
        };
        rebuild_seats();
    }

public:
    GameList game_list() const {
        return game_catalog_.list();
    }

    std::vector<ClientId> clients_for_game(const GameId& game_id) const {
        std::vector<ClientId> matching;
        for (const auto& [client_id, client] : clients_) {
            if (client.config.selected_game_id == game_id) {
                matching.push_back(client_id);
            }
        }

        std::sort(matching.begin(), matching.end());
        return matching;
    }

    const SeatAssignment& seats() const {
        return seats_;
    }

private:
    void rebuild_seats() {
        std::vector<ClientSeatRequest> requests;
        requests.reserve(clients_.size());

        for (const auto& [client_id, client] : clients_) {
            requests.push_back(ClientSeatRequest{client_id, client.config.requested_players});
        }

        std::sort(requests.begin(), requests.end(), [](const auto& lhs, const auto& rhs) {
            return lhs.client_id < rhs.client_id;
        });

        seats_ = seat_manager_.assign(requests);
        input_router_.set_seat_assignment(seats_);
    }

    InputRouter& input_router_;
    GameCatalog& game_catalog_;
    SeatManager seat_manager_;
    std::unordered_map<ClientId, ConnectedClient> clients_;
    SeatAssignment seats_;
};

} // namespace archstreamer
