#include "common/discovery.hpp"
#include "common/discovery_net.hpp"

#include <algorithm>
#include <string>
#include <vector>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>

namespace archstreamer {
namespace {

std::vector<std::string> base_broadcast_targets() {
    return {"255.255.255.255", "127.0.0.1"};
}

} // namespace

std::vector<std::string> ipv4_broadcast_targets() {
    auto targets = base_broadcast_targets();

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
    return targets;
}

std::vector<std::string> local_ipv4_addresses() {
    std::vector<std::string> addresses;

    ifaddrs* interfaces = nullptr;
    if (getifaddrs(&interfaces) != 0) {
        return addresses;
    }
    for (auto* entry = interfaces; entry != nullptr; entry = entry->ifa_next) {
        if (entry->ifa_addr == nullptr || entry->ifa_addr->sa_family != AF_INET) {
            continue;
        }
        if (entry->ifa_flags & IFF_LOOPBACK) {
            continue;
        }
        if ((entry->ifa_flags & IFF_UP) == 0) {
            continue;
        }
        char text[INET_ADDRSTRLEN]{};
        const auto* addr = reinterpret_cast<const sockaddr_in*>(entry->ifa_addr);
        if (inet_ntop(AF_INET, &addr->sin_addr, text, sizeof(text)) == nullptr) {
            continue;
        }
        const std::string value{text};
        if (std::find(addresses.begin(), addresses.end(), value) == addresses.end()) {
            addresses.push_back(value);
        }
    }
    freeifaddrs(interfaces);
    return addresses;
}

} // namespace archstreamer
