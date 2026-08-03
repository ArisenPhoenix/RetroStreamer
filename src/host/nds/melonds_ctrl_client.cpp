#include "host/nds/melonds_ctrl_client.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>

#if !defined(_WIN32)
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace archstreamer {

MelonDsCtrlClient::MelonDsCtrlClient(std::string server_name)
    : server_name_(std::move(server_name)) {}

#if defined(_WIN32)

bool MelonDsCtrlClient::send_command(
    std::string_view,
    std::chrono::milliseconds) const {
    last_error_ = "melonDS control socket is not implemented on Windows yet";
    return false;
}

#else

bool MelonDsCtrlClient::send_command(
    std::string_view command,
    std::chrono::milliseconds timeout) const {
    last_error_.clear();
    if (server_name_.empty()) {
        last_error_ = "empty melonDS control server name";
        return false;
    }

    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        last_error_ = "socket() failed";
        return false;
    }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    const std::string path = "/tmp/" + server_name_;
    if (path.size() >= sizeof(addr.sun_path)) {
        last_error_ = "control socket path too long";
        ::close(fd);
        return false;
    }
    std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path.c_str());

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    bool connected = false;
    while (std::chrono::steady_clock::now() < deadline) {
        if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
            connected = true;
            break;
        }
        if (errno != ECONNREFUSED && errno != ENOENT) {
            last_error_ = std::string("connect failed: ") + std::strerror(errno);
            ::close(fd);
            return false;
        }
        ::usleep(50 * 1000);
    }
    if (!connected) {
        last_error_ = "timed out connecting to " + path;
        ::close(fd);
        return false;
    }

    std::string payload(command);
    payload.push_back('\n');
    std::size_t written = 0;
    while (written < payload.size()) {
        const ssize_t n = ::write(fd, payload.data() + written, payload.size() - written);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            last_error_ = std::string("write failed: ") + std::strerror(errno);
            ::close(fd);
            return false;
        }
        written += static_cast<std::size_t>(n);
    }

    std::string reply;
    char buffer[256];
    while (std::chrono::steady_clock::now() < deadline) {
        pollfd pfd{};
        pfd.fd = fd;
        pfd.events = POLLIN;
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        const int pr = ::poll(&pfd, 1, static_cast<int>(std::max<std::chrono::milliseconds::rep>(
            remaining.count(), 0)));
        if (pr < 0) {
            if (errno == EINTR) {
                continue;
            }
            last_error_ = std::string("poll failed: ") + std::strerror(errno);
            ::close(fd);
            return false;
        }
        if (pr == 0) {
            break;
        }
        const ssize_t n = ::read(fd, buffer, sizeof(buffer));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            last_error_ = std::string("read failed: ") + std::strerror(errno);
            ::close(fd);
            return false;
        }
        if (n == 0) {
            break;
        }
        reply.append(buffer, static_cast<std::size_t>(n));
        if (reply.find('\n') != std::string::npos) {
            break;
        }
    }
    ::close(fd);

    while (!reply.empty() && (reply.back() == '\n' || reply.back() == '\r')) {
        reply.pop_back();
    }
    if (reply == "OK" || reply == "PONG") {
        return true;
    }
    last_error_ = reply.empty() ? "empty reply from melonDS ctrl" : reply;
    return false;
}

#endif

bool MelonDsCtrlClient::ping(std::chrono::milliseconds timeout) const {
    return send_command("PING", timeout);
}

bool MelonDsCtrlClient::lan_host(
    std::string_view player_name,
    int num_players,
    std::chrono::milliseconds timeout) const {
    std::string cmd = "LAN_HOST ";
    cmd.append(player_name);
    cmd.push_back(' ');
    cmd.append(std::to_string(num_players < 2 ? 2 : num_players));
    return send_command(cmd, timeout);
}

bool MelonDsCtrlClient::lan_connect(
    std::string_view player_name,
    std::string_view host,
    std::chrono::milliseconds timeout) const {
    std::string cmd = "LAN_CONNECT ";
    cmd.append(player_name);
    cmd.push_back(' ');
    cmd.append(host);
    return send_command(cmd, timeout);
}

bool MelonDsCtrlClient::lan_end(std::chrono::milliseconds timeout) const {
    return send_command("LAN_END", timeout);
}

} // namespace archstreamer
