#pragma once

#include "common/protocol.hpp"

#include <string>

namespace archstreamer {

struct ClientInfo {
    ClientId client_id = 0;
    std::string username;
};

} // namespace archstreamer
