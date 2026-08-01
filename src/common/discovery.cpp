#include "common/discovery.hpp"
#include "common/discovery_net.hpp"

#include <algorithm>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
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

bool parse_ipv4(std::string_view text, std::uint32_t& out_host_order) {
    if (text.empty() || text.size() >= INET_ADDRSTRLEN) {
        return false;
    }
    char buffer[INET_ADDRSTRLEN]{};
    std::memcpy(buffer, text.data(), text.size());
    in_addr addr{};
    if (inet_pton(AF_INET, buffer, &addr) != 1) {
        return false;
    }
    out_host_order = ntohl(addr.s_addr);
    return true;
}

bool is_loopback_ipv4(std::string_view address) {
    std::uint32_t host = 0;
    if (!parse_ipv4(address, host)) {
        return false;
    }
    return (host >> 24) == 127;
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

ByteBuffer serialize_host_probe() {
    ByteBuffer out;
    const auto* magic = DiscoveryProbeMagic;
    out.insert(out.end(), magic, magic + std::strlen(magic));
    return out;
}

bool is_host_probe(const ByteBuffer& bytes) {
    const auto magic_size = std::strlen(DiscoveryProbeMagic);
    return bytes.size() >= magic_size &&
        std::memcmp(bytes.data(), DiscoveryProbeMagic, magic_size) == 0;
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
    // Bind so we can answer unicast probes. Broadcast alone is often dropped on Wi‑Fi.
    socket_.bind_any(discovery_port_);
    socket_.set_nonblocking(true);
    socket_.enable_broadcast(true);
}

void HostDiscoveryAnnouncer::set_announcement(HostAnnouncement announcement) {
    announcement_ = std::move(announcement);
}

void HostDiscoveryAnnouncer::poll_probes() {
    const auto reply = serialize_host_announcement(announcement_);
    while (true) {
        const auto datagram = socket_.receive_from();
        if (!datagram.has_value()) {
            return;
        }
        if (!is_host_probe(datagram->bytes)) {
            continue;
        }
        // Reply to the client's discovery listen port (not the ephemeral probe source).
        try {
            socket_.send_to(reply, datagram->host, discovery_port_);
        } catch (...) {
        }
    }
}

void HostDiscoveryAnnouncer::advertise() {
    poll_probes();
    const auto packet = serialize_host_announcement(announcement_);
    for (const auto& target : ipv4_broadcast_targets()) {
        try {
            socket_.send_to(packet, target, discovery_port_);
        } catch (...) {
        }
    }
}

HostDiscoveryBrowser::HostDiscoveryBrowser(std::uint16_t discovery_port)
    : discovery_port_(discovery_port) {
    socket_.bind_any(discovery_port_);
    socket_.set_nonblocking(true);
    probe_socket_.set_nonblocking(true);
    next_subnet_probe_ = std::chrono::steady_clock::now();
}

void HostDiscoveryBrowser::set_seed_hosts(std::vector<std::string> hosts) {
    seed_hosts_ = std::move(hosts);
}

void HostDiscoveryBrowser::note_announcement(
    const HostAnnouncement& announcement,
    const std::string& address) {
    const auto now = std::chrono::steady_clock::now();
    // Keep one row per (username, source address). The same process may legitimately
    // appear on LAN, loopback, docker, and VPN IPs — each is a distinct connect path.
    auto existing = std::find_if(hosts_.begin(), hosts_.end(), [&](const DiscoveredHost& host) {
        return host.username == announcement.username && host.address == address;
    });
    if (existing != hosts_.end()) {
        existing->control_port = announcement.control_port;
        existing->input_port = announcement.input_port;
        existing->last_seen = now;
        return;
    }

    hosts_.push_back(DiscoveredHost{
        announcement.username,
        address,
        announcement.control_port,
        announcement.input_port,
        now,
    });
}

void HostDiscoveryBrowser::poll() {
    while (true) {
        const auto datagram = socket_.receive_from();
        if (!datagram.has_value()) {
            break;
        }
        const auto announcement = parse_host_announcement(datagram->bytes);
        if (!announcement.has_value()) {
            continue;
        }
        note_announcement(*announcement, datagram->host);
    }
    probe_lan();
}

std::vector<std::string> HostDiscoveryBrowser::probe_targets() const {
    std::vector<std::string> targets;
    auto add = [&](const std::string& address) {
        if (address.empty()) {
            return;
        }
        std::uint32_t host = 0;
        if (!parse_ipv4(address, host) || is_loopback_ipv4(address)) {
            return;
        }
        if (std::find(targets.begin(), targets.end(), address) == targets.end()) {
            targets.push_back(address);
        }
    };

    for (const auto& seed : seed_hosts_) {
        add(seed);
    }

    const auto now = std::chrono::steady_clock::now();
    // Full /24 sweeps are chatty — only when due.
    if (now < next_subnet_probe_) {
        return targets;
    }

    for (const auto& local : local_ipv4_addresses()) {
        std::uint32_t host = 0;
        if (!parse_ipv4(local, host)) {
            continue;
        }
        // Skip typical virtual/VPN ranges so we do not spray 250 probes into docker/libvirt.
        if ((host >> 16) == 0xac11u) { // 172.17.x.x
            continue;
        }
        if ((host & 0xffffff00u) == 0xc0a87a00u) { // 192.168.122.x
            continue;
        }
        if ((host >> 24) == 100u) { // 100.x.x.x (CGNAT / tailscale-like)
            continue;
        }
        const std::uint32_t base = host & 0xffffff00u;
        for (std::uint32_t octet = 1; octet <= 254; ++octet) {
            const std::uint32_t candidate = base | octet;
            if (candidate == host) {
                continue;
            }
            in_addr addr{};
            addr.s_addr = htonl(candidate);
            char text[INET_ADDRSTRLEN]{};
            if (inet_ntop(AF_INET, &addr, text, sizeof(text)) == nullptr) {
                continue;
            }
            add(text);
        }
    }
    return targets;
}

void HostDiscoveryBrowser::probe_lan() {
    const auto now = std::chrono::steady_clock::now();
    const bool subnet_due = now >= next_subnet_probe_;
    const auto targets = probe_targets();
    if (subnet_due) {
        next_subnet_probe_ = now + std::chrono::seconds(5);
    }
    if (targets.empty()) {
        return;
    }

    const auto probe = serialize_host_probe();
    for (const auto& target : targets) {
        try {
            probe_socket_.send_to(probe, target, discovery_port_);
        } catch (...) {
        }
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

bool ipv4_same_subnet_24(std::string_view left, std::string_view right) {
    std::uint32_t a = 0;
    std::uint32_t b = 0;
    if (!parse_ipv4(left, a) || !parse_ipv4(right, b)) {
        return false;
    }
    return (a & 0xffffff00u) == (b & 0xffffff00u);
}

std::optional<DiscoveredHost> prefer_discovered_host(const std::vector<DiscoveredHost>& hosts) {
    const auto local = local_ipv4_addresses();
    for (const auto& host : hosts) {
        if (is_loopback_ipv4(host.address)) {
            continue;
        }
        for (const auto& local_address : local) {
            if (ipv4_same_subnet_24(host.address, local_address)) {
                return host;
            }
        }
    }
    for (const auto& host : hosts) {
        if (!is_loopback_ipv4(host.address)) {
            return host;
        }
    }
    return std::nullopt;
}

} // namespace archstreamer
