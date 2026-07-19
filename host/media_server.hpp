#pragma once

#include "../common/media.hpp"
#include "../common/protocol.hpp"

namespace archstreamer {

class MediaServer {
public:
    virtual ~MediaServer() = default;

    virtual MediaEndpoint endpoint_for(ClientId client_id) const = 0;
    virtual void start() = 0;
    virtual void stop() = 0;
};

} // namespace archstreamer
