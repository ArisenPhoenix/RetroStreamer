#pragma once

#include "client/controller_manager.hpp"
#include "common/protocol.hpp"
#include "host/host_app_config.hpp"
#include "host/media_server.hpp"
#include "host/session_types.hpp"

#include <functional>
#include <optional>
#include <cstddef>

namespace archstreamer {

ClientId next_session_client_id(const SessionPlan& plan);
SessionClientConnection* disconnected_player_for_reconnect(SessionPlan& plan, const ClientHello& hello);
struct LiveSessionJoinTarget {
    ClientId client_id = 0;
    SessionClientConnection* reconnecting_client = nullptr;
};

struct LiveSessionJoinHandshake {
    TcpStream& stream;
    const ClientHello& hello;
    ClientId client_id = 0;
    SessionPlan& plan;
    const HostMediaPlanConfig& media_config;
    std::size_t& media_index;
    MediaServer& media_server;
};

LiveSessionJoinTarget resolve_live_session_join_target(
    SessionPlan& plan,
    const ClientHello& hello,
    bool reconnect_requested = false);

MediaEndpoint send_live_session_join_handshake(const LiveSessionJoinHandshake& join);

void reset_reconnected_session_client(
    SessionClientConnection& client,
    const ClientHello& hello,
    TcpStream&& stream,
    const SessionPlan& plan,
    const MediaEndpoint& endpoint);

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

/**
 * Accept one control connection (non-blocking). Handles ActiveSessionInfo / art / catalog.
 * On ClientHello, returns the hello + stream for the caller to route (new SP / multi / late join).
 * On LobbyPresence, returns presence credentials + stream for the catalog Connected hold.
 */
struct AcceptedControlHello {
    std::optional<AuthenticatedSessionRequest> client;
    std::optional<ControlClientConnection> presence;
};

std::optional<AcceptedControlHello> try_accept_control_hello(
    TcpListener& listener,
    const GameList& game_list,
    const std::filesystem::path& art_root,
    const std::function<ActiveSessionInfo()>& active_info_fn = {},
    const std::filesystem::path& save_root = {},
    bool allow_new_users = false);

} // namespace archstreamer
