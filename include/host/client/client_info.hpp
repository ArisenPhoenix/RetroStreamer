#pragma once

#include "common/protocol.hpp"

#include <string>

namespace archstreamer {

constexpr ClientId UnassignedClientId = static_cast<ClientId>(-1);

inline bool assigned_remote_client_id(ClientId client_id) {
    return client_id != HostClientId && client_id != UnassignedClientId;
}

struct ClientInfo {
    ClientId client_id = UnassignedClientId;
    std::uint64_t udp_session_token = 0;
    std::string username;
};

} // namespace archstreamer
