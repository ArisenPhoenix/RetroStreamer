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
std::optional<HostAnnouncement> parse_host_announcement(const ByteBuffer& bytes);

class HostDiscoveryAnnouncer {
public:
    HostDiscoveryAnnouncer(
        HostAnnouncement announcement,
        std::uint16_t discovery_port = DefaultDiscoveryPort);

    void set_announcement(HostAnnouncement announcement);
    void advertise();

private:
    HostAnnouncement announcement_;
    std::uint16_t discovery_port_;
    UdpSocket socket_;
};

class HostDiscoveryBrowser {
public:
    explicit HostDiscoveryBrowser(std::uint16_t discovery_port = DefaultDiscoveryPort);

    void poll();
    void expire_older_than(std::chrono::seconds max_age);
    std::vector<DiscoveredHost> hosts() const;

private:
    UdpSocket socket_;
    std::vector<DiscoveredHost> hosts_;
};

} // namespace archstreamer
