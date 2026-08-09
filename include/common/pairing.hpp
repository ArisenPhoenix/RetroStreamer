#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace archstreamer {

struct PairTarget {
    std::string ip;
    std::uint16_t port = 0;
    std::string token;
    std::string relay_host;
    std::uint16_t relay_port = 0;
};

std::string build_pair_uri(
    const std::string& ip,
    std::uint16_t port,
    const std::string& token,
    const std::string& relay_host = {},
    std::uint16_t relay_port = 0);

std::optional<PairTarget> parse_pair_uri(std::string_view uri, std::string* error = nullptr);

} // namespace archstreamer
