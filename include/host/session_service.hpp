#pragma once

#include "host/session_lobby.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>

namespace archstreamer {

class HostSessionService {
public:
    HostSessionService(
        std::uint16_t control_port,
        std::uint8_t max_clients,
        GameList game_list,
        std::chrono::seconds timeout,
        std::optional<ClientHello> host_hello = std::nullopt,
        std::function<bool()> should_stop = {},
        std::filesystem::path art_root = {});

    SessionPlan wait_for_ready_session() const;

private:
    std::uint16_t control_port_;
    std::uint8_t max_clients_;
    GameList game_list_;
    std::chrono::seconds timeout_;
    std::optional<ClientHello> host_hello_;
    std::function<bool()> should_stop_;
    std::filesystem::path art_root_;
};

} // namespace archstreamer
