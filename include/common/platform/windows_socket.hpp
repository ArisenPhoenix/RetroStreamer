#pragma once

#ifdef _WIN32

#include "common/protocol.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

namespace archstreamer {

void ensure_winsock_initialized();

class WindowsTcpStream {
public:
    WindowsTcpStream() = default;
    explicit WindowsTcpStream(SOCKET socket_fd, std::string peer_address = {});
    ~WindowsTcpStream();

    WindowsTcpStream(const WindowsTcpStream&) = delete;
    WindowsTcpStream& operator=(const WindowsTcpStream&) = delete;

    WindowsTcpStream(WindowsTcpStream&& other) noexcept;
    WindowsTcpStream& operator=(WindowsTcpStream&& other) noexcept;

    static WindowsTcpStream connect(const std::string& host, std::uint16_t port);

    void send_packet(const ByteBuffer& packet);
    ByteBuffer recv_packet();
    bool open() const;
    const std::string& peer_address() const;
    bool peer_closed() const;
    bool readable() const;

private:
    void write_all(const std::uint8_t* data, std::size_t size);
    bool read_all(std::uint8_t* data, std::size_t size);
    void close_if_open();

    SOCKET socket_ = INVALID_SOCKET;
    std::string peer_address_;
};

class WindowsTcpListener {
public:
    explicit WindowsTcpListener(std::uint16_t port);
    ~WindowsTcpListener();

    WindowsTcpListener(const WindowsTcpListener&) = delete;
    WindowsTcpListener& operator=(const WindowsTcpListener&) = delete;

    WindowsTcpStream accept_one();
    std::optional<WindowsTcpStream> accept_for(std::chrono::milliseconds timeout);

private:
    SOCKET socket_ = INVALID_SOCKET;
};

class WindowsUdpSocket {
public:
    struct UdpDatagram {
        ByteBuffer bytes;
        std::string host;
        std::uint16_t port = 0;
    };

    WindowsUdpSocket();
    ~WindowsUdpSocket();

    WindowsUdpSocket(const WindowsUdpSocket&) = delete;
    WindowsUdpSocket& operator=(const WindowsUdpSocket&) = delete;

    WindowsUdpSocket(WindowsUdpSocket&& other) noexcept;
    WindowsUdpSocket& operator=(WindowsUdpSocket&& other) noexcept;

    void bind_any(std::uint16_t port);
    void set_nonblocking(bool enabled);
    void enable_broadcast(bool enabled);
    void send_to(const ByteBuffer& bytes, const std::string& host, std::uint16_t port);
    std::optional<ByteBuffer> receive();
    std::optional<UdpDatagram> receive_from();

private:
    SOCKET socket_ = INVALID_SOCKET;
};

} // namespace archstreamer

#endif // _WIN32
