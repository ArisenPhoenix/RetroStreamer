#include "host/session/lobby.hpp"
#include "client/session_service.hpp"

#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

using namespace archstreamer;
using namespace std::chrono_literals;

namespace {

void expect_error(TcpStream& stream, const std::string& expected) {
    const auto packet = stream.receive_packet();
    if (!packet) {
        throw std::runtime_error("host closed without a rejection");
    }
    const auto payload = deserialize_packet(*packet);
    const auto* error = std::get_if<ErrorPacket>(&payload);
    if (!error || error->message != expected) {
        throw std::runtime_error("unexpected host rejection");
    }
}

TcpStream catalog_connection(std::uint16_t port) {
    auto stream = TcpStream::connect_to("127.0.0.1", port);
    stream.send_packet(serialize_packet(GameListRequest{}));
    const auto packet = stream.receive_packet();
    if (!packet || !std::holds_alternative<GameList>(deserialize_packet(*packet))) {
        throw std::runtime_error("expected catalog");
    }
    return stream;
}

} // namespace

int main() {
    try {
        std::unique_ptr<TcpListener> listener;
        std::uint16_t port = 0;
        for (std::uint16_t candidate = 47100; candidate < 47200; ++candidate) {
            try {
                listener = std::make_unique<TcpListener>(candidate);
                port = candidate;
                break;
            } catch (const std::exception&) {
            }
        }
        if (!listener) {
            throw std::runtime_error("no test port available");
        }
        std::atomic_bool stop{false};
        std::jthread server([&] {
            try {
                (void)gather_session_clients(*listener, 2, GameList{}, 10s, std::nullopt,
                    [&] { return stop.load(); });
            } catch (const std::exception&) {
                // The test stops the waiting lobby after exercising rejections.
            }
        });
        struct StopServer {
            std::atomic_bool& flag;
            ~StopServer() { flag = true; }
        } stop_server{stop};

        ClientHello hello;
        hello.username = "testuser";
        hello.password = "";
        auto bad_login = catalog_connection(port);
        PendingSession pending{SessionClient{}, std::move(bad_login), GameList{}, {}};
        bool rejection_reported = false;
        try {
            (void)ClientSessionService("127.0.0.1", port).finish_join(std::move(pending), hello);
        } catch (const std::exception& error) {
            rejection_reported = std::string(error.what()) ==
                "host rejected session: incorrect password or invalid credentials";
        }
        if (!rejection_reported) {
            throw std::runtime_error("client did not report host authentication rejection");
        }

        auto bad_presence = catalog_connection(port);
        LobbyPresence presence;
        presence.username = "testuser";
        presence.password = "";
        bad_presence.send_packet(serialize_packet(presence));
        expect_error(bad_presence, "incorrect password or invalid credentials");

        auto next_client = TcpStream::connect_to("127.0.0.1", port);
        next_client.send_packet(serialize_packet(ActiveSessionInfoRequest{}));
        const auto packet = next_client.receive_packet();
        if (!packet || !std::holds_alternative<ActiveSessionInfo>(deserialize_packet(*packet))) {
            throw std::runtime_error("lobby stopped accepting clients after rejection");
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
