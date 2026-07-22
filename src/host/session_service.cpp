#include "host/session_service.hpp"

#include <utility>

namespace archstreamer {

HostSessionService::HostSessionService(
    std::uint16_t control_port,
    std::uint8_t max_clients,
    GameList game_list,
    std::chrono::seconds timeout,
    std::optional<ClientHello> host_hello,
    std::function<bool()> should_stop,
    std::filesystem::path art_root)
    : control_port_(control_port),
      max_clients_(max_clients),
      game_list_(std::move(game_list)),
      timeout_(timeout),
      host_hello_(std::move(host_hello)),
      should_stop_(std::move(should_stop)),
      art_root_(std::move(art_root)) {
}

SessionPlan HostSessionService::wait_for_ready_session() const {
    return wait_for_session_clients(
        control_port_,
        max_clients_,
        game_list_,
        timeout_,
        host_hello_,
        should_stop_,
        art_root_);
}

} // namespace archstreamer
