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
    std::uint64_t packets_received_ = 0;
    std::uint64_t packets_applied_ = 0;
    bool logged_first_receive_ = false;
};

} // namespace archstreamer
