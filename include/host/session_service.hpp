#pragma once

#include "host/session_lobby.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>

namespace archstreamer {

class HostSessionService {
public:
    HostSessionService(
        std::uint16_t control_port,
        std::uint8_t max_clients,
        GameList game_list,
        std::chrono::seconds timeout,
        std::optional<ClientHello> host_hello = std::nullopt)
        : control_port_(control_port),
          max_clients_(max_clients),
          game_list_(std::move(game_list)),
          timeout_(timeout),
          host_hello_(std::move(host_hello)) {
    }

    SessionPlan wait_for_ready_session() const {
        return wait_for_session_clients(control_port_, max_clients_, game_list_, timeout_, host_hello_);
    }

    void notify_starting(SessionPlan& plan) const {
        send_session_starting_to_clients(plan);
    }

    void notify_ended(SessionPlan& plan, std::string_view reason) const {
        send_session_ended_to_clients(plan, reason);
    }

private:
    std::uint16_t control_port_;
    std::uint8_t max_clients_;
    GameList game_list_;
    std::chrono::seconds timeout_;
    std::optional<ClientHello> host_hello_;
};

} // namespace archstreamer
