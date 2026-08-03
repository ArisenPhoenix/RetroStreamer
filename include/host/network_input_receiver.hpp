#pragma once

#include "common/platform/default_platform.hpp"
#include "host/input_router.hpp"
#include "host/input_router_demux.hpp"

#include <atomic>
#include <thread>
#include <variant>

namespace archstreamer {

class NetworkInputReceiver {
public:
    NetworkInputReceiver(std::uint16_t port, InputRouter& input_router);
    NetworkInputReceiver(std::uint16_t port, InputRouterDemux& demux);
    ~NetworkInputReceiver();

    NetworkInputReceiver(const NetworkInputReceiver&) = delete;
    NetworkInputReceiver& operator=(const NetworkInputReceiver&) = delete;

    // Drain UDP on a dedicated thread so session/heartbeat work cannot stall pads.
    void start();
    void stop();

private:
    void poll();
    void thread_main();

    UdpSocket socket_;
    std::variant<InputRouter*, InputRouterDemux*> target_;
    std::uint64_t packets_received_ = 0;
    std::uint64_t packets_applied_ = 0;
    bool logged_first_receive_ = false;
    bool logged_first_nonzero_receive_ = false;
    std::atomic<bool> running_{false};
    std::thread worker_;
};

} // namespace archstreamer
