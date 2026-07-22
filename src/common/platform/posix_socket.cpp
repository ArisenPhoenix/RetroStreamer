#include "common/platform/posix_socket.hpp"

#include <array>
#include <cstring>
#include <stdexcept>
#include <utility>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

namespace archstreamer {

PosixTcpStream::PosixTcpStream(int fd) : fd_(fd) {
}

PosixTcpStream::PosixTcpStream(int fd, std::string peer_address)
    : fd_(fd), peer_address_(std::move(peer_address)) {
}

PosixTcpStream::~PosixTcpStream() {
    close_if_open();
}

PosixTcpStream::PosixTcpStream(PosixTcpStream&& other) noexcept
    : fd_(std::exchange(other.fd_, -1)), peer_address_(std::move(other.peer_address_)) {
}

PosixTcpStream& PosixTcpStream::operator=(PosixTcpStream&& other) noexcept {
    if (this != &other) {
        close_if_open();
        fd_ = std::exchange(other.fd_, -1);
        peer_address_ = std::move(other.peer_address_);
    }
    return *this;
}

PosixTcpStream PosixTcpStream::connect_to(const std::string& host, std::uint16_t port) {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        throw std::runtime_error("failed to create TCP socket");
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (inet_pton(AF_INET, host.c_str(), &address.sin_addr) != 1) {
        close(fd);
        throw std::runtime_error("invalid IPv4 address");
    }

    if (connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) < 0) {
        const auto message = std::string("failed to connect TCP socket: ") + std::strerror(errno);
        close(fd);
        throw std::runtime_error(message);
    }

    return PosixTcpStream(fd, host);
}

void PosixTcpStream::send_packet(const ByteBuffer& packet) {
    write_all(packet.data(), packet.size());
}

std::optional<ByteBuffer> PosixTcpStream::receive_packet() {
    constexpr std::size_t header_size = sizeof(std::uint32_t) + sizeof(std::uint16_t) + sizeof(PacketType) + sizeof(std::uint32_t);
    std::array<std::uint8_t, header_size> header{};
    if (!read_all(header.data(), header.size())) {
        return std::nullopt;
    }

    Reader reader(header);
    const auto magic = reader.read_pod<std::uint32_t>();
    const auto version = reader.read_pod<std::uint16_t>();
    reader.read_pod<PacketType>();
    const auto payload_size = reader.read_pod<std::uint32_t>();
    if (magic != ProtocolMagic) {
        throw std::runtime_error("bad TCP protocol magic");
    }
    if (version != ProtocolVersion) {
        throw std::runtime_error("unsupported TCP protocol version");
    }

    ByteBuffer packet(header.begin(), header.end());
    packet.resize(header_size + payload_size);
    if (payload_size > 0 && !read_all(packet.data() + header_size, payload_size)) {
        throw std::runtime_error("TCP stream ended mid-packet");
    }

    return packet;
}

bool PosixTcpStream::open() const {
    return fd_ >= 0;
}

const std::string& PosixTcpStream::peer_address() const {
    return peer_address_;
}

bool PosixTcpStream::peer_closed() const {
    if (fd_ < 0) {
        return true;
    }

    if (!readable()) {
        return false;
    }

    std::uint8_t byte = 0;
    while (true) {
        const auto result = recv(fd_, &byte, sizeof(byte), MSG_PEEK | MSG_DONTWAIT);
        if (result == 0) {
            return true;
        }
        if (result > 0) {
            return false;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return false;
        }

        return true;
    }
}

bool PosixTcpStream::readable() const {
    if (fd_ < 0) {
        return false;
    }

    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(fd_, &read_fds);

    timeval tv{};
    const int ready = select(fd_ + 1, &read_fds, nullptr, nullptr, &tv);
    return ready > 0;
}

void PosixTcpStream::write_all(const std::uint8_t* data, std::size_t size) {
    std::size_t written = 0;
    while (written < size) {
        const auto result = write(fd_, data + written, size - written);
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw std::runtime_error(std::string("failed to write TCP stream: ") + std::strerror(errno));
        }
        if (result == 0) {
            throw std::runtime_error("TCP stream closed while writing");
        }
        written += static_cast<std::size_t>(result);
    }
}

bool PosixTcpStream::read_all(std::uint8_t* data, std::size_t size) {
    std::size_t read_count = 0;
    while (read_count < size) {
        const auto result = read(fd_, data + read_count, size - read_count);
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw std::runtime_error(std::string("failed to read TCP stream: ") + std::strerror(errno));
        }
        if (result == 0) {
            return false;
        }
        read_count += static_cast<std::size_t>(result);
    }

    return true;
}

void PosixTcpStream::close_if_open() {
    if (fd_ >= 0) {
        close(fd_);
        fd_ = -1;
    }
}

PosixTcpListener::PosixTcpListener(std::uint16_t port) {
    fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0) {
        throw std::runtime_error("failed to create TCP listener");
    }

    int enabled = 1;
    setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);
    if (bind(fd_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) < 0) {
        const auto message = std::string("failed to bind TCP listener: ") + std::strerror(errno);
        close(fd_);
        fd_ = -1;
        throw std::runtime_error(message);
    }
    if (listen(fd_, 8) < 0) {
        const auto message = std::string("failed to listen on TCP socket: ") + std::strerror(errno);
        close(fd_);
        fd_ = -1;
        throw std::runtime_error(message);
    }
}

PosixTcpListener::~PosixTcpListener() {
    if (fd_ >= 0) {
        close(fd_);
    }
}

PosixTcpStream PosixTcpListener::accept_one() {
    while (true) {
        sockaddr_in peer{};
        socklen_t peer_length = sizeof(peer);
        const int client_fd = accept(fd_, reinterpret_cast<sockaddr*>(&peer), &peer_length);
        if (client_fd >= 0) {
            char address[INET_ADDRSTRLEN] = {};
            const char* result = inet_ntop(AF_INET, &peer.sin_addr, address, sizeof(address));
            return PosixTcpStream(client_fd, result == nullptr ? "" : address);
        }
        if (errno != EINTR) {
            throw std::runtime_error(std::string("failed to accept TCP connection: ") + std::strerror(errno));
        }
    }
}

std::optional<PosixTcpStream> PosixTcpListener::accept_for(std::chrono::milliseconds timeout) {
    if (timeout.count() < 0) {
        timeout = std::chrono::milliseconds(0);
    }

    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(fd_, &read_fds);

    timeval tv{};
    tv.tv_sec = static_cast<time_t>(timeout.count() / 1000);
    tv.tv_usec = static_cast<suseconds_t>((timeout.count() % 1000) * 1000);

    while (true) {
        auto local_fds = read_fds;
        auto local_tv = tv;
        const int ready = select(fd_ + 1, &local_fds, nullptr, nullptr, &local_tv);
        if (ready == 0) {
            return std::nullopt;
        }
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw std::runtime_error(std::string("failed while waiting for TCP connection: ") + std::strerror(errno));
        }

        return accept_one();
    }
}

PosixUdpSocket::PosixUdpSocket() {
    fd_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd_ < 0) {
        throw std::runtime_error("failed to create UDP socket");
    }
}

PosixUdpSocket::~PosixUdpSocket() {
    if (fd_ >= 0) {
        close(fd_);
    }
}

PosixUdpSocket::PosixUdpSocket(PosixUdpSocket&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {
}

PosixUdpSocket& PosixUdpSocket::operator=(PosixUdpSocket&& other) noexcept {
    if (this != &other) {
        if (fd_ >= 0) {
            close(fd_);
        }
        fd_ = std::exchange(other.fd_, -1);
    }
    return *this;
}

void PosixUdpSocket::bind_any(std::uint16_t port) {
    const int reuse = 1;
    if (setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        throw std::runtime_error("failed to set SO_REUSEADDR on UDP socket");
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);

    if (bind(fd_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) < 0) {
        throw std::runtime_error("failed to bind UDP socket");
    }
}

void PosixUdpSocket::set_nonblocking(bool enabled) {
    const int flags = fcntl(fd_, F_GETFL, 0);
    if (flags < 0) {
        throw std::runtime_error("failed to read UDP socket flags");
    }

    const int next_flags = enabled ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    if (fcntl(fd_, F_SETFL, next_flags) < 0) {
        throw std::runtime_error("failed to update UDP socket flags");
    }
}

void PosixUdpSocket::enable_broadcast(bool enabled) {
    const int value = enabled ? 1 : 0;
    if (setsockopt(fd_, SOL_SOCKET, SO_BROADCAST, &value, sizeof(value)) < 0) {
        throw std::runtime_error("failed to configure UDP broadcast");
    }
}

void PosixUdpSocket::send_to(const ByteBuffer& bytes, const std::string& host, std::uint16_t port) {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (inet_pton(AF_INET, host.c_str(), &address.sin_addr) != 1) {
        throw std::runtime_error("invalid IPv4 address");
    }

    const auto sent = sendto(fd_, bytes.data(), bytes.size(), 0, reinterpret_cast<const sockaddr*>(&address), sizeof(address));
    if (sent < 0 || static_cast<std::size_t>(sent) != bytes.size()) {
        throw std::runtime_error("failed to send UDP packet");
    }
}

std::optional<ByteBuffer> PosixUdpSocket::receive() {
    const auto datagram = receive_from();
    if (!datagram.has_value()) {
        return std::nullopt;
    }
    return datagram->bytes;
}

std::optional<PosixUdpSocket::UdpDatagram> PosixUdpSocket::receive_from() {
    std::array<std::uint8_t, 2048> buffer{};
    sockaddr_in source{};
    socklen_t source_length = sizeof(source);
    const auto received = recvfrom(
        fd_,
        buffer.data(),
        buffer.size(),
        0,
        reinterpret_cast<sockaddr*>(&source),
        &source_length);
    if (received < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return std::nullopt;
        }

        throw std::runtime_error("failed to receive UDP packet");
    }

    char host[INET_ADDRSTRLEN]{};
    if (inet_ntop(AF_INET, &source.sin_addr, host, sizeof(host)) == nullptr) {
        throw std::runtime_error("failed to read UDP source address");
    }

    return UdpDatagram{
        ByteBuffer(buffer.begin(), buffer.begin() + received),
        host,
        ntohs(source.sin_port),
    };
}

} // namespace archstreamer
