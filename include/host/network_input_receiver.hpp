#pragma once

#include "common/platform/default_platform.hpp"
#include "host/input_router.hpp"

namespace archstreamer {

class NetworkInputReceiver {
public:
    NetworkInputReceiver(std::uint16_t port, InputRouter& input_router);

    void poll();

private:
    UdpSocket socket_;
    InputRouter& input_router_;
};

} // namespace archstreamer
