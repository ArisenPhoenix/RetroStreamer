#pragma once

#include "common/serialization.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

namespace archstreamer {

class PosixTcpStream {
public:
    PosixTcpStream() = default;
    explicit PosixTcpStream(int fd);
    PosixTcpStream(int fd, std::string peer_address);
    ~PosixTcpStream();

    PosixTcpStream(const PosixTcpStream&) = delete;
    PosixTcpStream& operator=(const PosixTcpStream&) = delete;

    PosixTcpStream(PosixTcpStream&& other) noexcept;
    PosixTcpStream& operator=(PosixTcpStream&& other) noexcept;

    static PosixTcpStream connect_to(const std::string& host, std::uint16_t port);

    void send_packet(const ByteBuffer& packet);
    std::optional<ByteBuffer> receive_packet();
    bool open() const;
    const std::string& peer_address() const;
    bool peer_closed() const;
    bool readable() const;

private:
    void write_all(const std::uint8_t* data, std::size_t size);
    bool read_all(std::uint8_t* data, std::size_t size);
    void close_if_open();

    int fd_ = -1;
    std::string peer_address_;
};

class PosixTcpListener {
public:
    explicit PosixTcpListener(std::uint16_t port);
    ~PosixTcpListener();

    PosixTcpListener(const PosixTcpListener&) = delete;
    PosixTcpListener& operator=(const PosixTcpListener&) = delete;

    PosixTcpStream accept_one();
    std::optional<PosixTcpStream> accept_for(std::chrono::milliseconds timeout);

private:
    int fd_ = -1;
};

class PosixUdpSocket {
public:
    struct UdpDatagram {
        ByteBuffer bytes;
        std::string host;
        std::uint16_t port = 0;
    };

    PosixUdpSocket();
    ~PosixUdpSocket();

    PosixUdpSocket(const PosixUdpSocket&) = delete;
    PosixUdpSocket& operator=(const PosixUdpSocket&) = delete;

    PosixUdpSocket(PosixUdpSocket&& other) noexcept;
    PosixUdpSocket& operator=(PosixUdpSocket&& other) noexcept;

    void bind_any(std::uint16_t port);
    void set_nonblocking(bool enabled);
    void enable_broadcast(bool enabled);
    void send_to(const ByteBuffer& bytes, const std::string& host, std::uint16_t port);
    std::optional<ByteBuffer> receive();
    std::optional<UdpDatagram> receive_from();

private:
    int fd_ = -1;
};

} // namespace archstreamer
