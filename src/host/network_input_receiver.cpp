#include "common/serialization.hpp"
#include "host/network_input_receiver.hpp"

#include <chrono>
#include <iostream>
#include <thread>
#include <variant>

namespace archstreamer {

NetworkInputReceiver::NetworkInputReceiver(std::uint16_t port, InputRouter& input_router)
    : input_router_(input_router) {
    socket_.bind_any(port);
    socket_.set_nonblocking(true);
    std::cout << "Receiving UDP controller input on port " << port << '\n';
}

NetworkInputReceiver::~NetworkInputReceiver() {
    stop();
}

void NetworkInputReceiver::start() {
    if (running_.exchange(true)) {
        return;
    }
    worker_ = std::thread([this] { thread_main(); });
}

void NetworkInputReceiver::stop() {
    if (!running_.exchange(false)) {
        return;
    }
    if (worker_.joinable()) {
        worker_.join();
    }
}

void NetworkInputReceiver::thread_main() {
    while (running_.load(std::memory_order_relaxed)) {
        poll();
        // Change-filtered uinput means a short sleep is enough for latency without
        // burning a core. ~500 µs ≈ 2 kHz sample ceiling.
        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
}

void NetworkInputReceiver::poll() {
    while (true) {
        const auto bytes = socket_.receive();
        if (!bytes.has_value()) {
            return;
        }

        ++packets_received_;
        try {
            auto payload = deserialize_packet(*bytes);
            if (auto* input = std::get_if<ControllerInput>(&payload); input != nullptr) {
                if (!logged_first_receive_) {
                    logged_first_receive_ = true;
                    std::cout
                        << "First controller UDP packet received from client "
                        << static_cast<int>(input->client_id)
                        << " (local P" << static_cast<int>(input->local_player) + 1
                        << ", buttons=0x" << std::hex << input->state.buttons << std::dec
                        << ")\n";
                }
                if (input_router_.route(*input)) {
                    ++packets_applied_;
                } else if (packets_applied_ == 0 && packets_received_ <= 5) {
                    std::cerr
                        << "Controller input from client "
                        << static_cast<int>(input->client_id)
                        << " local P" << static_cast<int>(input->local_player) + 1
                        << " has no seat assignment; ignored\n";
                }
            }
        } catch (const std::exception& error) {
            std::cerr << "Ignoring bad input packet: " << error.what() << '\n';
        }
    }
}

} // namespace archstreamer
