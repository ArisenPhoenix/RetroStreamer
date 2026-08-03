#include "host/retroarch_netcmd.hpp"

#include "common/platform/default_platform.hpp"

#include <chrono>
#include <string>
#include <thread>

namespace archstreamer {

bool send_retroarch_netcmd(std::string_view command, std::uint16_t port, std::string_view host) {
    if (command.empty()) {
        return false;
    }
    try {
        UdpSocket socket;
        ByteBuffer payload(command.begin(), command.end());
        socket.send_to(payload, std::string(host), port);
        return true;
    } catch (...) {
        return false;
    }
}

std::optional<bool> query_retroarch_paused(std::uint16_t port, std::string_view host) {
    try {
        UdpSocket socket;
        socket.bind_any(0);
        socket.set_nonblocking(true);
        const std::string command = "GET_STATUS";
        ByteBuffer payload(command.begin(), command.end());
        socket.send_to(payload, std::string(host), port);

        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(120);
        while (std::chrono::steady_clock::now() < deadline) {
            if (const auto reply = socket.receive(); reply.has_value() && !reply->empty()) {
                const std::string text(reply->begin(), reply->end());
                // GET_STATUS PAUSED ...  /  GET_STATUS PLAYING ...
                if (text.find("PAUSED") != std::string::npos) {
                    return true;
                }
                if (text.find("PLAYING") != std::string::npos) {
                    return false;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    } catch (...) {
        return std::nullopt;
    }
    return std::nullopt;
}

bool set_retroarch_paused(bool want_paused, std::uint16_t port, std::string_view host) {
    const auto current = query_retroarch_paused(port, host);
    if (current.has_value() && *current == want_paused) {
        return true;
    }
    // Unknown status: only toggle when we want pause (avoid unpausing a
    // playing game on a failed query). When we know status differs, toggle.
    if (!current.has_value() && !want_paused) {
        return false;
    }
    return send_retroarch_netcmd("PAUSE_TOGGLE", port, host);
}

} // namespace archstreamer
