#pragma once

#include "input_router.hpp"
#include "seat_manager.hpp"

#include <algorithm>
#include <unordered_map>
#include <vector>

namespace archstreamer {

struct ConnectedClient {
    ClientId client_id = 0;
    ClientConfig config;
};

class SessionServer {
public:
    explicit SessionServer(InputRouter& input_router) : input_router_(input_router) {}

    HostWelcome connect(ClientId client_id, const ClientHello& hello) {
        ClientConfig config;
        config.requested_players = hello.requested_players;
        config.wants_video = hello.wants_video;
        config.wants_audio = hello.wants_audio;
        set_client_config(client_id, config);

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
        clients_[client_id] = ConnectedClient{client_id, config};
        rebuild_seats();
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
    SeatManager seat_manager_;
    std::unordered_map<ClientId, ConnectedClient> clients_;
    SeatAssignment seats_;
};

} // namespace archstreamer
