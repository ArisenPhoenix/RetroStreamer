#include "common/discovery.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "common/platform/windows_socket.hpp"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#else
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#endif

namespace archstreamer {

namespace {

void append_u16(ByteBuffer& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
    out.push_back(static_cast<std::uint8_t>(value & 0xff));
}

std::optional<std::uint16_t> read_u16(const ByteBuffer& bytes, std::size_t& offset) {
    if (offset + 2 > bytes.size()) {
        return std::nullopt;
    }
    const auto value = static_cast<std::uint16_t>((bytes[offset] << 8) | bytes[offset + 1]);
    offset += 2;
    return value;
}

std::optional<std::string> read_string(const ByteBuffer& bytes, std::size_t& offset) {
    const auto length = read_u16(bytes, offset);
    if (!length.has_value() || offset + *length > bytes.size()) {
        return std::nullopt;
    }
    std::string value(reinterpret_cast<const char*>(bytes.data() + offset), *length);
    offset += *length;
    return value;
}

std::vector<std::string> ipv4_broadcast_targets() {
    std::vector<std::string> targets{"255.255.255.255"};

#ifdef _WIN32
    ensure_winsock_initialized();

    ULONG buffer_size = 16 * 1024;
    std::vector<unsigned char> buffer(buffer_size);
    auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
    ULONG flags = GAA_FLAG_INCLUDE_PREFIX | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG result = GetAdaptersAddresses(AF_INET, flags, nullptr, adapters, &buffer_size);
    if (result == ERROR_BUFFER_OVERFLOW) {
        buffer.resize(buffer_size);
        adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
        result = GetAdaptersAddresses(AF_INET, flags, nullptr, adapters, &buffer_size);
    }
    if (result != NO_ERROR) {
        return targets;
    }

    for (auto* adapter = adapters; adapter != nullptr; adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp || adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) {
            continue;
        }
        for (auto* unicast = adapter->FirstUnicastAddress; unicast != nullptr; unicast = unicast->Next) {
            if (unicast->Address.lpSockaddr == nullptr || unicast->Address.lpSockaddr->sa_family != AF_INET) {
                continue;
            }
            const auto* addr = reinterpret_cast<const sockaddr_in*>(unicast->Address.lpSockaddr);
            const auto prefix = unicast->OnLinkPrefixLength;
            if (prefix > 32) {
                continue;
            }
            const std::uint32_t host = ntohl(addr->sin_addr.s_addr);
            const std::uint32_t mask = prefix == 0 ? 0u : (0xffffffffu << (32 - prefix));
            const std::uint32_t broadcast = (host & mask) | ~mask;
            in_addr broadcast_addr{};
            broadcast_addr.s_addr = htonl(broadcast);
            char text[INET_ADDRSTRLEN]{};
            if (inet_ntop(AF_INET, &broadcast_addr, text, sizeof(text)) == nullptr) {
                continue;
            }
            const std::string value{text};
            if (std::find(targets.begin(), targets.end(), value) == targets.end()) {
                targets.push_back(value);
            }
        }
    }
#else
    ifaddrs* interfaces = nullptr;
    if (getifaddrs(&interfaces) != 0) {
        return targets;
    }

    for (auto* entry = interfaces; entry != nullptr; entry = entry->ifa_next) {
        if (entry->ifa_addr == nullptr || entry->ifa_addr->sa_family != AF_INET) {
            continue;
        }
        if (entry->ifa_flags & IFF_LOOPBACK) {
            continue;
        }
        if ((entry->ifa_flags & IFF_UP) == 0 || (entry->ifa_flags & IFF_BROADCAST) == 0) {
            continue;
        }
        if (entry->ifa_broadaddr == nullptr) {
            continue;
        }

        char broadcast[INET_ADDRSTRLEN]{};
        const auto* addr = reinterpret_cast<const sockaddr_in*>(entry->ifa_broadaddr);
        if (inet_ntop(AF_INET, &addr->sin_addr, broadcast, sizeof(broadcast)) == nullptr) {
            continue;
        }
        const std::string value{broadcast};
        if (std::find(targets.begin(), targets.end(), value) == targets.end()) {
            targets.push_back(value);
        }
    }

    freeifaddrs(interfaces);
#endif
    return targets;
}

} // namespace

ByteBuffer serialize_host_announcement(const HostAnnouncement& announcement) {
    ByteBuffer out;
    const auto* magic = DiscoveryMagic;
    out.insert(out.end(), magic, magic + std::strlen(magic));
    append_u16(out, static_cast<std::uint16_t>(announcement.username.size()));
    out.insert(
        out.end(),
        reinterpret_cast<const std::uint8_t*>(announcement.username.data()),
        reinterpret_cast<const std::uint8_t*>(announcement.username.data()) + announcement.username.size());
    append_u16(out, announcement.control_port);
    append_u16(out, announcement.input_port);
    return out;
}

std::optional<HostAnnouncement> parse_host_announcement(const ByteBuffer& bytes) {
    const auto magic_size = std::strlen(DiscoveryMagic);
    if (bytes.size() < magic_size) {
        return std::nullopt;
    }
    if (std::memcmp(bytes.data(), DiscoveryMagic, magic_size) != 0) {
        return std::nullopt;
    }

    std::size_t offset = magic_size;
    const auto username = read_string(bytes, offset);
    const auto control_port = read_u16(bytes, offset);
    const auto input_port = read_u16(bytes, offset);
    if (!username.has_value() || !control_port.has_value() || !input_port.has_value()) {
        return std::nullopt;
    }
    if (username->empty()) {
        return std::nullopt;
    }

    return HostAnnouncement{*username, *control_port, *input_port};
}

HostDiscoveryAnnouncer::HostDiscoveryAnnouncer(HostAnnouncement announcement, std::uint16_t discovery_port)
    : announcement_(std::move(announcement)), discovery_port_(discovery_port) {
    socket_.enable_broadcast(true);
}

void HostDiscoveryAnnouncer::set_announcement(HostAnnouncement announcement) {
    announcement_ = std::move(announcement);
}

void HostDiscoveryAnnouncer::advertise() {
    const auto packet = serialize_host_announcement(announcement_);
    for (const auto& target : ipv4_broadcast_targets()) {
        socket_.send_to(packet, target, discovery_port_);
    }
}

HostDiscoveryBrowser::HostDiscoveryBrowser(std::uint16_t discovery_port) {
    socket_.bind_any(discovery_port);
    socket_.set_nonblocking(true);
}

void HostDiscoveryBrowser::poll() {
    while (true) {
        const auto datagram = socket_.receive_from();
        if (!datagram.has_value()) {
            return;
        }

        const auto announcement = parse_host_announcement(datagram->bytes);
        if (!announcement.has_value()) {
            continue;
        }

        const auto now = std::chrono::steady_clock::now();
        auto existing = std::find_if(hosts_.begin(), hosts_.end(), [&](const DiscoveredHost& host) {
            return host.username == announcement->username && host.address == datagram->host;
        });
        if (existing != hosts_.end()) {
            existing->control_port = announcement->control_port;
            existing->input_port = announcement->input_port;
            existing->last_seen = now;
            continue;
        }

        hosts_.push_back(DiscoveredHost{
            announcement->username,
            datagram->host,
            announcement->control_port,
            announcement->input_port,
            now,
        });
    }
}

void HostDiscoveryBrowser::expire_older_than(std::chrono::seconds max_age) {
    const auto now = std::chrono::steady_clock::now();
    hosts_.erase(
        std::remove_if(hosts_.begin(), hosts_.end(), [&](const DiscoveredHost& host) {
            return now - host.last_seen > max_age;
        }),
        hosts_.end());
}

std::vector<DiscoveredHost> HostDiscoveryBrowser::hosts() const {
    return hosts_;
}

} // namespace archstreamer
