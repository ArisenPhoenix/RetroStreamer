#include "client/session_service.hpp"
#include "client/game_filter.hpp"
#include "host/session/lobby.hpp"

#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>

using namespace archstreamer;
using namespace std::chrono_literals;

int main() {
    try {
        GameInfo single_player_game;
        single_player_game.supports_multiplayer = true;
        single_player_game.max_players = 1;
        GameFilter multiplayer_filter;
        multiplayer_filter.mode = GameFilterMode::Multiplayer;
        if (game_matches_filter(single_player_game, multiplayer_filter)) {
            throw std::runtime_error("one-player game passed multiplayer filter");
        }
        SessionPlan invalid_plan;
        invalid_plan.game.session_mode = GameSessionMode::Multiplayer;
        bool invalid_rejected = false;
        try {
            (void)launch_requirements_satisfied(invalid_plan, single_player_game);
        } catch (const std::exception&) {
            invalid_rejected = true;
        }
        if (!invalid_rejected) throw std::runtime_error("host accepted impossible multiplayer game");

        std::unique_ptr<TcpListener> listener;
        std::uint16_t port = 0;
        for (std::uint16_t candidate = 47200; candidate < 47300; ++candidate) {
            try {
                listener = std::make_unique<TcpListener>(candidate);
                port = candidate;
                break;
            } catch (const std::exception&) {
            }
        }
        if (!listener) throw std::runtime_error("no test port available");

        std::atomic_bool entered_lobby{false};
        std::atomic_bool server_finished{false};
        std::atomic_bool cancel{false};
        std::jthread server([&] {
            try {
                auto socket = listener->accept_one();
                const auto packet = socket.receive_packet();
                if (!packet) return;
                auto hello = std::get<ClientHello>(deserialize_packet(*packet));
                GameInfo game;
                game.id = "game";
                game.max_players = 2;
                GameList catalog;
                catalog.games.push_back(game);
                auto first = make_session_client(1, std::move(hello), std::move(socket));
                entered_lobby = true;
                (void)gather_session_clients(*listener, 2, catalog, 10s, std::nullopt,
                    {}, {}, std::move(first));
            } catch (const std::exception&) {
            }
            server_finished = true;
        });

        auto socket = TcpStream::connect_to("127.0.0.1", port);
        PendingSession pending{SessionClient{}, std::move(socket), GameList{}, {}};
        ClientHello hello;
        hello.username = "testuser";
        hello.password = "test";
        hello.selected_game_id = "game";
        hello.session_mode = GameSessionMode::Multiplayer;
        hello.requested_players = 1;
        std::jthread canceller([&] {
            while (!entered_lobby.load()) std::this_thread::sleep_for(10ms);
            cancel = true;
        });
        bool cancelled = false;
        try {
            (void)ClientSessionService("127.0.0.1", port).finish_join(
                std::move(pending), hello, {}, [&] { return cancel.load(); });
        } catch (const std::exception& error) {
            cancelled = std::string(error.what()) == "client stopped";
        }
        if (!cancelled) throw std::runtime_error("waiting client did not cancel");
        for (int i = 0; i < 200 && !server_finished.load(); ++i) std::this_thread::sleep_for(10ms);
        if (!server_finished.load()) throw std::runtime_error("lobby did not release disconnected client");
        server.join();

        std::jthread invalid_server([&] {
            try {
                auto incoming = listener->accept_one();
                auto packet = incoming.receive_packet();
                if (!packet) return;
                auto candidate = std::get<ClientHello>(deserialize_packet(*packet));
                auto first = make_session_client(2, std::move(candidate), std::move(incoming));
                single_player_game.id = "game";
                GameList catalog;
                catalog.games.push_back(single_player_game);
                (void)gather_session_clients(*listener, 2, catalog, 10s, std::nullopt,
                    {}, {}, std::move(first));
            } catch (const std::exception&) {
            }
        });
        auto invalid_socket = TcpStream::connect_to("127.0.0.1", port);
        PendingSession invalid_pending{SessionClient{}, std::move(invalid_socket), GameList{}, {}};
        bool received_rejection = false;
        try {
            (void)ClientSessionService("127.0.0.1", port).finish_join(std::move(invalid_pending), hello);
        } catch (const std::exception& error) {
            received_rejection = std::string(error.what()) ==
                "host rejected session: selected game supports fewer than two players";
        }
        if (!received_rejection) throw std::runtime_error("first client did not receive game rejection");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
