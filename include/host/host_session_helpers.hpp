#pragma once

#include "client/controller_manager.hpp"
#include "common/protocol.hpp"
#include "host/host_app_config.hpp"
#include "host/media_server.hpp"
#include "host/session_service.hpp"

#include <cstddef>

namespace archstreamer {

ClientId next_session_client_id(const SessionPlan& plan);
SessionClientConnection* disconnected_player_for_reconnect(SessionPlan& plan, const ClientHello& hello);

std::string hex_vid_pid(std::uint16_t vendor_id, std::uint16_t product_id);
std::optional<std::string> sdl_ignore_list_for_session(const SessionPlan& plan);
HostPlayerControllerIdentity host_player_controller_identity(const ControllerDevice& device);

void poll_active_session_joins(
    TcpListener& listener,
    SessionPlan& plan,
    const GameList& game_list,
    const HostAppConfig& config,
    std::size_t& media_index,
    MediaServer& media_server);

} // namespace archstreamer
