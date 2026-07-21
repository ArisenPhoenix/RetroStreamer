#pragma once

#include "host/session_lobby.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>

namespace archstreamer {

class HostSessionService {
public:
    HostSessionService(
        std::uint16_t control_port,
        std::uint8_t max_clients,
        GameList game_list,
        std::chrono::seconds timeout,
        std::optional<ClientHello> host_hello = std::nullopt,
        std::function<bool()> should_stop = {});

    SessionPlan wait_for_ready_session() const;

private:
    std::uint16_t control_port_;
    std::uint8_t max_clients_;
    GameList game_list_;
    std::chrono::seconds timeout_;
    std::optional<ClientHello> host_hello_;
    std::function<bool()> should_stop_;
};

} // namespace archstreamer
