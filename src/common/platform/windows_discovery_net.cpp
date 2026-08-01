#include "common/discovery.hpp"
#include "common/discovery_net.hpp"

#include "common/platform/windows_socket.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace archstreamer {
namespace {

std::vector<std::string> base_broadcast_targets() {
    return {"255.255.255.255", "127.0.0.1"};
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

std::vector<std::string> ipv4_broadcast_targets() {
    auto targets = base_broadcast_targets();
    ensure_winsock_initialized();

    ULONG buffer_size = 16 * 1024;
    std::vector<unsigned char> buffer(buffer_size);
    auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
    ULONG flags =
        GAA_FLAG_INCLUDE_PREFIX | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
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

    return targets;
}

std::vector<std::string> local_ipv4_addresses() {
    std::vector<std::string> addresses;

    ULONG size = 0;
    if (GetAdaptersAddresses(
            AF_INET,
            GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
            nullptr,
            nullptr,
            &size) != ERROR_BUFFER_OVERFLOW ||
        size == 0) {
        return addresses;
    }
    std::vector<std::uint8_t> buffer(size);
    auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
    if (GetAdaptersAddresses(
            AF_INET,
            GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
            nullptr,
            adapters,
            &size) != NO_ERROR) {
        return addresses;
    }
    for (auto* adapter = adapters; adapter != nullptr; adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp) {
            continue;
        }
        for (auto* unicast = adapter->FirstUnicastAddress; unicast != nullptr; unicast = unicast->Next) {
            if (unicast->Address.lpSockaddr == nullptr ||
                unicast->Address.lpSockaddr->sa_family != AF_INET) {
                continue;
            }
            const auto* addr = reinterpret_cast<const sockaddr_in*>(unicast->Address.lpSockaddr);
            char text[INET_ADDRSTRLEN]{};
            if (inet_ntop(AF_INET, &addr->sin_addr, text, sizeof(text)) == nullptr) {
                continue;
            }
            const std::string value{text};
            if (is_loopback_ipv4(value)) {
                continue;
            }
            if (std::find(addresses.begin(), addresses.end(), value) == addresses.end()) {
                addresses.push_back(value);
            }
        }
    }

    return addresses;
}

} // namespace archstreamer
