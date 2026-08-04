#pragma once

#include "common/protocol.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace archstreamer {

/**
 * Talk to a patched melonDS --archstreamer-ctrl QLocalServer.
 * Linux: connects to /tmp/<server_name>. Windows: not wired yet.
 *
 * Touch uses a persistent Unix socket so move events stay cheap.
 */
class MelonDsCtrlClient {
public:
    explicit MelonDsCtrlClient(std::string server_name);
    MelonDsCtrlClient(const MelonDsCtrlClient&) = delete;
    MelonDsCtrlClient& operator=(const MelonDsCtrlClient&) = delete;
    MelonDsCtrlClient(MelonDsCtrlClient&& other) noexcept;
    MelonDsCtrlClient& operator=(MelonDsCtrlClient&& other) noexcept;
    ~MelonDsCtrlClient();

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

    /** Absolute DS stylus (0–255 × 0–191). Keeps a persistent ctrl connection. */
    bool touch(std::uint16_t x, std::uint16_t y);
    bool touch_end();
    void close_touch_channel();

    /**
     * Query top/bottom screen AABBs in melonDS window pixels (SCREENS).
     * Updates with swap/emphasis; optional top rect is reported when present.
     */
    bool query_screens(
        DsScreenLayout& out,
        std::chrono::milliseconds timeout = std::chrono::milliseconds(500)) const;

    const std::string& server_name() const { return server_name_; }
    const std::string& last_error() const { return last_error_; }

private:
    bool send_command(
        std::string_view command,
        std::chrono::milliseconds timeout) const;
    std::optional<std::string> transact(
        std::string_view command,
        std::chrono::milliseconds timeout) const;
    bool ensure_touch_connected(std::chrono::milliseconds timeout);
    bool write_touch_line(std::string_view command);

    std::string server_name_;
    mutable std::string last_error_;
#if !defined(_WIN32)
    int touch_fd_ = -1;
#endif
};

} // namespace archstreamer
