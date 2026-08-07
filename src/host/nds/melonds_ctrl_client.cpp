#include "host/nds/melonds_ctrl_client.hpp"

#include <algorithm>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstring>
#include <sstream>
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

MelonDsCtrlClient::MelonDsCtrlClient(MelonDsCtrlClient&& other) noexcept
    : server_name_(std::move(other.server_name_))
    , last_error_(std::move(other.last_error_))
#if !defined(_WIN32)
    , touch_fd_(other.touch_fd_)
#endif
{
#if !defined(_WIN32)
    other.touch_fd_ = -1;
#endif
}

MelonDsCtrlClient& MelonDsCtrlClient::operator=(MelonDsCtrlClient&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    close_touch_channel();
    server_name_ = std::move(other.server_name_);
    last_error_ = std::move(other.last_error_);
#if !defined(_WIN32)
    touch_fd_ = other.touch_fd_;
    other.touch_fd_ = -1;
#endif
    return *this;
}

MelonDsCtrlClient::~MelonDsCtrlClient() {
    close_touch_channel();
}

namespace {

std::int16_t clamp_i16(int value) {
    if (value < INT16_MIN) {
        return static_cast<std::int16_t>(INT16_MIN);
    }
    if (value > INT16_MAX) {
        return static_cast<std::int16_t>(INT16_MAX);
    }
    return static_cast<std::int16_t>(value);
}

std::uint16_t clamp_u16(int value) {
    if (value < 0) {
        return 0;
    }
    if (value > UINT16_MAX) {
        return static_cast<std::uint16_t>(UINT16_MAX);
    }
    return static_cast<std::uint16_t>(value);
}

} // namespace

#if defined(_WIN32)

std::optional<std::string> MelonDsCtrlClient::transact(
    std::string_view,
    std::chrono::milliseconds) const {
    last_error_ = "melonDS control socket is not implemented on Windows yet";
    return std::nullopt;
}

bool MelonDsCtrlClient::send_command(
    std::string_view command,
    std::chrono::milliseconds timeout) const {
    return transact(command, timeout).has_value();
}

bool MelonDsCtrlClient::ensure_touch_connected(std::chrono::milliseconds) {
    last_error_ = "melonDS control socket is not implemented on Windows yet";
    return false;
}

bool MelonDsCtrlClient::write_touch_line(std::string_view) {
    last_error_ = "melonDS control socket is not implemented on Windows yet";
    return false;
}

void MelonDsCtrlClient::close_touch_channel() {}

#else

std::optional<std::string> MelonDsCtrlClient::transact(
    std::string_view command,
    std::chrono::milliseconds timeout) const {
    last_error_.clear();
    if (server_name_.empty()) {
        last_error_ = "empty melonDS control server name";
        return std::nullopt;
    }

    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        last_error_ = "socket() failed";
        return std::nullopt;
    }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    const std::string path = "/tmp/" + server_name_;
    if (path.size() >= sizeof(addr.sun_path)) {
        last_error_ = "control socket path too long";
        ::close(fd);
        return std::nullopt;
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
            return std::nullopt;
        }
        ::usleep(50 * 1000);
    }
    if (!connected) {
        last_error_ = "timed out connecting to " + path;
        ::close(fd);
        return std::nullopt;
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
            return std::nullopt;
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
            return std::nullopt;
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
            return std::nullopt;
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
    if (reply.empty()) {
        last_error_ = "empty reply from melonDS ctrl";
        return std::nullopt;
    }
    return reply;
}

bool MelonDsCtrlClient::send_command(
    std::string_view command,
    std::chrono::milliseconds timeout) const {
    const auto reply = transact(command, timeout);
    if (!reply.has_value()) {
        return false;
    }
    if (*reply == "OK" || *reply == "PONG" || reply->rfind("OK ", 0) == 0) {
        return true;
    }
    last_error_ = *reply;
    return false;
}

bool MelonDsCtrlClient::ensure_touch_connected(std::chrono::milliseconds timeout) {
    if (touch_fd_ >= 0) {
        return true;
    }
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
    while (std::chrono::steady_clock::now() < deadline) {
        if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
            touch_fd_ = fd;
            return true;
        }
        if (errno != ECONNREFUSED && errno != ENOENT) {
            last_error_ = std::string("connect failed: ") + std::strerror(errno);
            ::close(fd);
            return false;
        }
        ::usleep(20 * 1000);
    }
    last_error_ = "timed out connecting touch channel to " + path;
    ::close(fd);
    return false;
}

bool MelonDsCtrlClient::write_touch_line(std::string_view command) {
    if (!ensure_touch_connected(std::chrono::milliseconds(500))) {
        return false;
    }
    std::string payload(command);
    payload.push_back('\n');
    std::size_t written = 0;
    while (written < payload.size()) {
        const ssize_t n = ::write(touch_fd_, payload.data() + written, payload.size() - written);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            last_error_ = std::string("touch write failed: ") + std::strerror(errno);
            close_touch_channel();
            return false;
        }
        written += static_cast<std::size_t>(n);
    }

    // Non-blocking drain so replies don't fill the socket buffer during move spam.
    const int flags = ::fcntl(touch_fd_, F_GETFL, 0);
    if (flags >= 0) {
        ::fcntl(touch_fd_, F_SETFL, flags | O_NONBLOCK);
    }
    char buffer[256];
    while (true) {
        const ssize_t n = ::read(touch_fd_, buffer, sizeof(buffer));
        if (n > 0) {
            continue;
        }
        if (n == 0) {
            close_touch_channel();
            last_error_ = "touch channel closed";
            return false;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        last_error_ = std::string("touch drain failed: ") + std::strerror(errno);
        close_touch_channel();
        return false;
    }
    if (flags >= 0) {
        ::fcntl(touch_fd_, F_SETFL, flags);
    }
    return true;
}

void MelonDsCtrlClient::close_touch_channel() {
    if (touch_fd_ >= 0) {
        ::close(touch_fd_);
        touch_fd_ = -1;
    }
}

#endif

bool MelonDsCtrlClient::ping(std::chrono::milliseconds timeout) const {
    return send_command("PING", timeout);
}

bool MelonDsCtrlClient::lan_host(
    std::string_view player_name,
    int num_players,
    std::chrono::milliseconds timeout) const {
    const std::string cmd =
        "LAN_HOST " + std::string(player_name) + " " + std::to_string(num_players);
    return send_command(cmd, timeout);
}

bool MelonDsCtrlClient::lan_connect(
    std::string_view player_name,
    std::string_view host,
    std::chrono::milliseconds timeout) const {
    const std::string cmd =
        "LAN_CONNECT " + std::string(player_name) + " " + std::string(host);
    return send_command(cmd, timeout);
}

bool MelonDsCtrlClient::lan_end(std::chrono::milliseconds timeout) const {
    return send_command("LAN_END", timeout);
}

bool MelonDsCtrlClient::touch(std::uint16_t x, std::uint16_t y) {
    if (x > 255) {
        x = 255;
    }
    if (y > 191) {
        y = 191;
    }
    const std::string cmd = "TOUCH " + std::to_string(x) + " " + std::to_string(y);
    return write_touch_line(cmd);
}

bool MelonDsCtrlClient::touch_end() {
    return write_touch_line("TOUCH_END");
}

bool MelonDsCtrlClient::query_screens(
    DsScreenLayout& out,
    std::chrono::milliseconds timeout) const {
    const auto reply = transact("SCREENS", timeout);
    if (!reply.has_value()) {
        return false;
    }
    if (reply->rfind("OK ", 0) != 0) {
        last_error_ = *reply;
        return false;
    }

    // OK <ww> <wh> <hasTop> <tx> <ty> <tw> <th> <hasBot> <bx> <by> <bw> <bh>
    std::istringstream in(reply->substr(3));
    int ww = 0;
    int wh = 0;
    int has_top = 0;
    int tx = 0;
    int ty = 0;
    int tw = 0;
    int th = 0;
    int has_bot = 0;
    int bx = 0;
    int by = 0;
    int bw = 0;
    int bh = 0;
    if (!(in >> ww >> wh >> has_top >> tx >> ty >> tw >> th >> has_bot >> bx >> by >> bw >> bh)) {
        last_error_ = "malformed SCREENS reply: " + *reply;
        return false;
    }

    out = {};
    out.window_w = clamp_u16(ww);
    out.window_h = clamp_u16(wh);
    out.has_top = has_top != 0;
    out.top_x = clamp_i16(tx);
    out.top_y = clamp_i16(ty);
    out.top_w = clamp_i16(tw);
    out.top_h = clamp_i16(th);
    out.has_bot = has_bot != 0;
    out.bot_x = clamp_i16(bx);
    out.bot_y = clamp_i16(by);
    out.bot_w = clamp_i16(bw);
    out.bot_h = clamp_i16(bh);
    return out.has_top || out.has_bot;
}

bool MelonDsCtrlClient::set_paused(bool paused, std::chrono::milliseconds timeout) const {
    return send_command(paused ? "PAUSE on" : "PAUSE off", timeout);
}

bool MelonDsCtrlClient::toggle_paused(std::chrono::milliseconds timeout) const {
    return send_command("PAUSE toggle", timeout);
}

std::optional<bool> MelonDsCtrlClient::query_paused(std::chrono::milliseconds timeout) const {
    const auto reply = transact("PAUSE", timeout);
    if (!reply.has_value()) {
        return std::nullopt;
    }
    if (reply->rfind("OK ", 0) != 0) {
        last_error_ = *reply;
        return std::nullopt;
    }
    const auto rest = reply->substr(3);
    if (rest == "1" || rest == "true" || rest == "on") {
        return true;
    }
    if (rest == "0" || rest == "false" || rest == "off") {
        return false;
    }
    last_error_ = "malformed PAUSE reply: " + *reply;
    return std::nullopt;
}

} // namespace archstreamer
