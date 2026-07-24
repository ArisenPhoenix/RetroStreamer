#pragma once

#include "common/platform/default_platform.hpp"
#include "host/input_router.hpp"

#include <atomic>
#include <thread>

namespace archstreamer {

class NetworkInputReceiver {
public:
    NetworkInputReceiver(std::uint16_t port, InputRouter& input_router);
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
    InputRouter& input_router_;
    std::uint64_t packets_received_ = 0;
    std::uint64_t packets_applied_ = 0;
    bool logged_first_receive_ = false;
    std::atomic<bool> running_{false};
    std::thread worker_;
};

} // namespace archstreamer
