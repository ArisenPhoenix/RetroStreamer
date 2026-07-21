#include "common/serialization.hpp"
#include "host/network_input_receiver.hpp"

#include <iostream>
#include <variant>

namespace archstreamer {

NetworkInputReceiver::NetworkInputReceiver(std::uint16_t port, InputRouter& input_router)
    : input_router_(input_router) {
    socket_.bind_any(port);
    socket_.set_nonblocking(true);
    std::cout << "Receiving UDP controller input on port " << port << '\n';
}

void NetworkInputReceiver::poll() {
    while (true) {
        const auto bytes = socket_.receive();
        if (!bytes.has_value()) {
            return;
        }

        try {
            auto payload = deserialize_packet(*bytes);
            if (auto* input = std::get_if<ControllerInput>(&payload); input != nullptr) {
                input_router_.route(*input);
            }
        } catch (const std::exception& error) {
            std::cerr << "Ignoring bad input packet: " << error.what() << '\n';
        }
    }
}

} // namespace archstreamer
