#include "common/platform/windows_socket.hpp"

#ifdef _WIN32

#include "common/binary.hpp"

#include <array>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

#pragma comment(lib, "ws2_32.lib")

namespace archstreamer {
namespace {

std::once_flag g_winsock_once;

[[noreturn]] void throw_winsock(const char* what) {
    throw std::runtime_error(std::string(what) + " (WSA " + std::to_string(WSAGetLastError()) + ")");
}

} // namespace

void ensure_winsock_initialized() {
    std::call_once(g_winsock_once, [] {
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            throw_winsock("WSAStartup failed");
        }
    });
}

WindowsTcpStream::WindowsTcpStream(SOCKET socket_fd, std::string peer_address)
    : socket_(socket_fd), peer_address_(std::move(peer_address)) {
}

WindowsTcpStream::~WindowsTcpStream() {
    close_if_open();
}

WindowsTcpStream::WindowsTcpStream(WindowsTcpStream&& other) noexcept
    : socket_(std::exchange(other.socket_, INVALID_SOCKET)), peer_address_(std::move(other.peer_address_)) {
}

WindowsTcpStream& WindowsTcpStream::operator=(WindowsTcpStream&& other) noexcept {
    if (this != &other) {
        close_if_open();
        socket_ = std::exchange(other.socket_, INVALID_SOCKET);
        peer_address_ = std::move(other.peer_address_);
    }
    return *this;
}

WindowsTcpStream WindowsTcpStream::connect(const std::string& host, std::uint16_t port) {
    ensure_winsock_initialized();

    SOCKET fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd == INVALID_SOCKET) {
        throw_winsock("failed to create TCP socket");
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (inet_pton(AF_INET, host.c_str(), &address.sin_addr) != 1) {
        closesocket(fd);
        throw std::runtime_error("invalid IPv4 address");
    }

    if (::connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        closesocket(fd);
        throw_winsock("failed to connect TCP socket");
    }

    return WindowsTcpStream(fd, host);
}

void WindowsTcpStream::send_packet(const ByteBuffer& packet) {
    write_all(packet.data(), packet.size());
}

ByteBuffer WindowsTcpStream::recv_packet() {
    constexpr std::size_t header_size = 10;
    ByteBuffer header(header_size);
    if (!read_all(header.data(), header.size())) {
        throw std::runtime_error("TCP stream ended before packet header");
    }

    BinaryReader reader(header);
    const auto magic = reader.read_pod<std::uint32_t>();
    const auto version = reader.read_pod<std::uint16_t>();
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

bool WindowsTcpStream::open() const {
    return socket_ != INVALID_SOCKET;
}

const std::string& WindowsTcpStream::peer_address() const {
    return peer_address_;
}

bool WindowsTcpStream::peer_closed() const {
    if (socket_ == INVALID_SOCKET) {
        return true;
    }
    if (!readable()) {
        return false;
    }

    char byte = 0;
    const int result = recv(socket_, &byte, 1, MSG_PEEK);
    if (result == 0) {
        return true;
    }
    if (result > 0) {
        return false;
    }
    const int err = WSAGetLastError();
    if (err == WSAEWOULDBLOCK) {
        return false;
    }
    return true;
}

bool WindowsTcpStream::readable() const {
    if (socket_ == INVALID_SOCKET) {
        return false;
    }

    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(socket_, &read_fds);

    timeval tv{};
    const int ready = select(0, &read_fds, nullptr, nullptr, &tv);
    return ready > 0;
}

void WindowsTcpStream::write_all(const std::uint8_t* data, std::size_t size) {
    std::size_t written = 0;
    while (written < size) {
        const int result = send(socket_, reinterpret_cast<const char*>(data + written), static_cast<int>(size - written), 0);
        if (result == SOCKET_ERROR) {
            throw_winsock("failed to write TCP stream");
        }
        if (result == 0) {
            throw std::runtime_error("TCP stream closed while writing");
        }
        written += static_cast<std::size_t>(result);
    }
}

bool WindowsTcpStream::read_all(std::uint8_t* data, std::size_t size) {
    std::size_t read_count = 0;
    while (read_count < size) {
        const int result = recv(socket_, reinterpret_cast<char*>(data + read_count), static_cast<int>(size - read_count), 0);
        if (result == SOCKET_ERROR) {
            throw_winsock("failed to read TCP stream");
        }
        if (result == 0) {
            return false;
        }
        read_count += static_cast<std::size_t>(result);
    }
    return true;
}

void WindowsTcpStream::close_if_open() {
    if (socket_ != INVALID_SOCKET) {
        closesocket(socket_);
        socket_ = INVALID_SOCKET;
    }
}

WindowsTcpListener::WindowsTcpListener(std::uint16_t port) {
    ensure_winsock_initialized();

    socket_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket_ == INVALID_SOCKET) {
        throw_winsock("failed to create TCP listener");
    }

    BOOL enabled = TRUE;
    setsockopt(socket_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&enabled), sizeof(enabled));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);
    if (bind(socket_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        closesocket(socket_);
        socket_ = INVALID_SOCKET;
        throw_winsock("failed to bind TCP listener");
    }
    if (listen(socket_, 8) != 0) {
        closesocket(socket_);
        socket_ = INVALID_SOCKET;
        throw_winsock("failed to listen on TCP socket");
    }
}

WindowsTcpListener::~WindowsTcpListener() {
    if (socket_ != INVALID_SOCKET) {
        closesocket(socket_);
    }
}

WindowsTcpStream WindowsTcpListener::accept_one() {
    sockaddr_in peer{};
    int peer_length = sizeof(peer);
    const SOCKET client = accept(socket_, reinterpret_cast<sockaddr*>(&peer), &peer_length);
    if (client == INVALID_SOCKET) {
        throw_winsock("failed to accept TCP connection");
    }

    char address[INET_ADDRSTRLEN] = {};
    const char* result = inet_ntop(AF_INET, &peer.sin_addr, address, sizeof(address));
    return WindowsTcpStream(client, result == nullptr ? "" : address);
}

std::optional<WindowsTcpStream> WindowsTcpListener::accept_for(std::chrono::milliseconds timeout) {
    if (timeout.count() < 0) {
        timeout = std::chrono::milliseconds(0);
    }

    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(socket_, &read_fds);

    timeval tv{};
    tv.tv_sec = static_cast<long>(timeout.count() / 1000);
    tv.tv_usec = static_cast<long>((timeout.count() % 1000) * 1000);

    const int ready = select(0, &read_fds, nullptr, nullptr, &tv);
    if (ready == 0) {
        return std::nullopt;
    }
    if (ready < 0) {
        throw_winsock("failed while waiting for TCP connection");
    }
    return accept_one();
}

WindowsUdpSocket::WindowsUdpSocket() {
    ensure_winsock_initialized();
    socket_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_ == INVALID_SOCKET) {
        throw_winsock("failed to create UDP socket");
    }
}

WindowsUdpSocket::~WindowsUdpSocket() {
    if (socket_ != INVALID_SOCKET) {
        closesocket(socket_);
    }
}

WindowsUdpSocket::WindowsUdpSocket(WindowsUdpSocket&& other) noexcept
    : socket_(std::exchange(other.socket_, INVALID_SOCKET)) {
}

WindowsUdpSocket& WindowsUdpSocket::operator=(WindowsUdpSocket&& other) noexcept {
    if (this != &other) {
        if (socket_ != INVALID_SOCKET) {
            closesocket(socket_);
        }
        socket_ = std::exchange(other.socket_, INVALID_SOCKET);
    }
    return *this;
}

void WindowsUdpSocket::bind_any(std::uint16_t port) {
    BOOL reuse = TRUE;
    if (setsockopt(socket_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse)) != 0) {
        throw_winsock("failed to set SO_REUSEADDR on UDP socket");
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);

    if (bind(socket_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        throw_winsock("failed to bind UDP socket");
    }
}

void WindowsUdpSocket::set_nonblocking(bool enabled) {
    u_long mode = enabled ? 1 : 0;
    if (ioctlsocket(socket_, FIONBIO, &mode) != 0) {
        throw_winsock("failed to update UDP socket flags");
    }
}

void WindowsUdpSocket::enable_broadcast(bool enabled) {
    BOOL value = enabled ? TRUE : FALSE;
    if (setsockopt(socket_, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&value), sizeof(value)) != 0) {
        throw_winsock("failed to configure UDP broadcast");
    }
}

void WindowsUdpSocket::send_to(const ByteBuffer& bytes, const std::string& host, std::uint16_t port) {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (inet_pton(AF_INET, host.c_str(), &address.sin_addr) != 1) {
        throw std::runtime_error("invalid IPv4 address");
    }

    const int sent = sendto(
        socket_,
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<int>(bytes.size()),
        0,
        reinterpret_cast<const sockaddr*>(&address),
        sizeof(address));
    if (sent == SOCKET_ERROR || static_cast<std::size_t>(sent) != bytes.size()) {
        throw_winsock("failed to send UDP packet");
    }
}

std::optional<ByteBuffer> WindowsUdpSocket::receive() {
    const auto datagram = receive_from();
    if (!datagram.has_value()) {
        return std::nullopt;
    }
    return datagram->bytes;
}

std::optional<WindowsUdpSocket::UdpDatagram> WindowsUdpSocket::receive_from() {
    std::array<std::uint8_t, 2048> buffer{};
    sockaddr_in source{};
    int source_length = sizeof(source);
    const int received = recvfrom(
        socket_,
        reinterpret_cast<char*>(buffer.data()),
        static_cast<int>(buffer.size()),
        0,
        reinterpret_cast<sockaddr*>(&source),
        &source_length);
    if (received == SOCKET_ERROR) {
        if (WSAGetLastError() == WSAEWOULDBLOCK) {
            return std::nullopt;
        }
        throw_winsock("failed to receive UDP packet");
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

#endif // _WIN32
