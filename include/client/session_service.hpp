#pragma once

#include "client/session_client.hpp"
#include "common/platform/default_platform.hpp"

#include <filesystem>
#include <string>
#include <optional>

namespace archstreamer {

PacketPayload receive_client_control_payload(TcpStream& stream);

struct JoinedSession {
    SessionClient session;
    TcpStream stream;
    GameList game_list;
    SessionReady ready;
};

struct PendingSession {
    SessionClient session;
    TcpStream stream;
    GameList game_list;
};

struct SessionStart {
    SessionStarting starting;
    std::optional<MediaEndpoint> media_endpoint;
};

class ClientSessionService {
public:
    ClientSessionService(std::string host, std::uint16_t control_port);
    ClientSessionService(std::string host, std::uint16_t control_port, std::filesystem::path catalog_cache_path);

    PendingSession begin() const;
    ActiveSessionInfo active_session_info() const;
    JoinedSession finish_join(PendingSession pending, const ClientHello& hello) const;
    SessionStart wait_for_starting(TcpStream& stream) const;

private:
    std::string host_;
    std::uint16_t control_port_;
    std::filesystem::path catalog_cache_path_;
};

} // namespace archstreamer
