#include "common/serialization.hpp"
#include "host/network_input_receiver.hpp"

#include <chrono>
#include <iostream>
#include <thread>
#include <variant>

namespace archstreamer {

NetworkInputReceiver::NetworkInputReceiver(std::uint16_t port, InputRouter& input_router)
    : target_(&input_router) {
    socket_.bind_any(port);
    socket_.set_nonblocking(true);
    std::cout << "Receiving UDP controller input on port " << port << '\n';
}

NetworkInputReceiver::NetworkInputReceiver(std::uint16_t port, InputRouterDemux& demux)
    : target_(&demux) {
    socket_.bind_any(port);
    socket_.set_nonblocking(true);
    std::cout << "Receiving UDP controller input on port " << port << " (demux)\n";
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
                bool applied = false;
                if (auto* router = std::get_if<InputRouter*>(&target_); router != nullptr) {
                    applied = (*router)->route(*input);
                } else if (auto* demux = std::get_if<InputRouterDemux*>(&target_); demux != nullptr) {
                    applied = (*demux)->route(*input);
                }
                if (applied) {
                    ++packets_applied_;
                } else if (packets_applied_ == 0 && packets_received_ <= 20) {
                    std::cout
                        << "Controller input from client "
                        << static_cast<int>(input->client_id)
                        << " local P" << static_cast<int>(input->local_player) + 1
                        << " has no seat assignment; ignored\n";
                }
            } else if (auto* keys = std::get_if<KeyboardInput>(&payload); keys != nullptr) {
                if (auto* router = std::get_if<InputRouter*>(&target_); router != nullptr) {
                    (*router)->route(*keys);
                } else if (auto* demux = std::get_if<InputRouterDemux*>(&target_); demux != nullptr) {
                    (*demux)->route(*keys);
                }
            }
        } catch (const std::exception& error) {
            std::cerr << "Ignoring bad input packet: " << error.what() << '\n';
        }
    }
}

} // namespace archstreamer
