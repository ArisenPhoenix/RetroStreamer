#include "host/retroarch_netcmd.hpp"

#include "common/platform/default_platform.hpp"

#include <string>

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

} // namespace archstreamer
