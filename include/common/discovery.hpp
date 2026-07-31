#pragma once

#include "common/platform/default_platform.hpp"
#include "common/serialization.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace archstreamer {

inline constexpr std::uint16_t DefaultDiscoveryPort = 45550;
inline constexpr const char* DiscoveryMagic = "ASDISC01";
// Client→host unicast probe. Needed when Wi‑Fi APs drop UDP broadcasts
// (common on Public/"guest" networks): gameplay unicast still works, browse does not.
inline constexpr const char* DiscoveryProbeMagic = "ASDISCQ1";

struct HostAnnouncement {
    std::string username;
    std::uint16_t control_port = 45555;
    std::uint16_t input_port = 45454;
};

struct DiscoveredHost {
    std::string username;
    std::string address;
    std::uint16_t control_port = 45555;
    std::uint16_t input_port = 45454;
    std::chrono::steady_clock::time_point last_seen{};
};

ByteBuffer serialize_host_announcement(const HostAnnouncement& announcement);
ByteBuffer serialize_host_probe();
bool is_host_probe(const ByteBuffer& bytes);
std::optional<HostAnnouncement> parse_host_announcement(const ByteBuffer& bytes);

class HostDiscoveryAnnouncer {
public:
    HostDiscoveryAnnouncer(
        HostAnnouncement announcement,
        std::uint16_t discovery_port = DefaultDiscoveryPort);

    void set_announcement(HostAnnouncement announcement);
    // Broadcast announce (may be dropped on filtered Wi‑Fi) + answer unicast probes.
    void advertise();
    void poll_probes();

private:
    HostAnnouncement announcement_;
    std::uint16_t discovery_port_;
    UdpSocket socket_;
};

class HostDiscoveryBrowser {
public:
    explicit HostDiscoveryBrowser(std::uint16_t discovery_port = DefaultDiscoveryPort);

    // Optional last-known host IPs to probe first (e.g. saved client/hostAddress).
    void set_seed_hosts(std::vector<std::string> hosts);
    void poll();
    // Send unicast probes across local /24 subnets (and seeds). Safe to call often;
    // full subnet sweep is rate-limited internally.
    void probe_lan();
    void expire_older_than(std::chrono::seconds max_age);
    std::vector<DiscoveredHost> hosts() const;

private:
    void note_announcement(const HostAnnouncement& announcement, const std::string& address);
    std::vector<std::string> probe_targets() const;

    UdpSocket socket_;
    UdpSocket probe_socket_;
    std::uint16_t discovery_port_ = DefaultDiscoveryPort;
    std::vector<DiscoveredHost> hosts_;
    std::vector<std::string> seed_hosts_;
    std::chrono::steady_clock::time_point next_subnet_probe_{};
};

/** Non-loopback IPv4 addresses on local interfaces (for same-subnet host ranking). */
std::vector<std::string> local_ipv4_addresses();

/** True when both addresses are IPv4 and share the same /24 prefix. */
bool ipv4_same_subnet_24(std::string_view left, std::string_view right);

/**
 * Prefer a discovered LAN host on the same /24 as a local interface.
 * Skips loopback; falls back to the first non-loopback host, else nullopt.
 */
std::optional<DiscoveredHost> prefer_discovered_host(const std::vector<DiscoveredHost>& hosts);

} // namespace archstreamer
