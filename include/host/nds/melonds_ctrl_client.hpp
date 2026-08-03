#pragma once

#include <chrono>
#include <string>
#include <string_view>

namespace archstreamer {

/**
 * Talk to a patched melonDS --archstreamer-ctrl QLocalServer.
 * Linux: connects to /tmp/<server_name>. Windows: not wired yet.
 */
class MelonDsCtrlClient {
public:
    explicit MelonDsCtrlClient(std::string server_name);

    bool ping(std::chrono::milliseconds timeout = std::chrono::milliseconds(1500)) const;
    bool lan_host(
        std::string_view player_name,
        int num_players = 2,
        std::chrono::milliseconds timeout = std::chrono::milliseconds(3000)) const;
    bool lan_connect(
        std::string_view player_name,
        std::string_view host = "127.0.0.1",
        std::chrono::milliseconds timeout = std::chrono::milliseconds(3000)) const;
    bool lan_end(std::chrono::milliseconds timeout = std::chrono::milliseconds(2000)) const;

    const std::string& server_name() const { return server_name_; }
    const std::string& last_error() const { return last_error_; }

private:
    bool send_command(
        std::string_view command,
        std::chrono::milliseconds timeout) const;

    std::string server_name_;
    mutable std::string last_error_;
};

} // namespace archstreamer
