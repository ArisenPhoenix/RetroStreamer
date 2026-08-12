#pragma once

#include "common/protocol.hpp"

#include <optional>
#include <string>
#include <vector>

namespace archstreamer {

class SessionClient {
public:
    ClientHello make_hello(
        std::string username,
        std::string display_name,
        std::optional<GameId> selected_game_id,
        GameSessionMode session_mode,
        std::uint8_t requested_players,
        std::vector<ControllerInfo> controllers = {},
        bool wants_video = true,
        bool wants_audio = true,
        std::string password = {}) const;

    ClientConfig make_config(
        std::optional<std::string> username,
        std::optional<std::string> display_name,
        std::optional<GameId> selected_game_id,
        GameSessionMode session_mode,
        std::uint8_t requested_players,
        std::vector<ControllerInfo> controllers = {},
        bool wants_video = true,
        bool wants_audio = true) const;

    GameListRequest make_game_list_request(std::uint64_t client_catalog_revision = 0) const;

    void apply_welcome(const HostWelcome& welcome);
    void apply_game_list(GameList game_list);
    void apply_seats(SeatAssignment seats);

    std::optional<ClientId> client_id() const;
    std::uint64_t udp_session_token() const;
    const SeatAssignment& seats() const;
    const GameList& game_list() const;

private:
    std::optional<ClientId> client_id_;
    std::uint64_t udp_session_token_ = 0;
    GameList game_list_;
    SeatAssignment seats_;
};

} // namespace archstreamer
