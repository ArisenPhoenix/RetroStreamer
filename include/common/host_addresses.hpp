#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace archstreamer {

/** True when [value] looks like IPv4 (dotted decimal) or IPv6 (contains ':'). */
inline bool looks_like_ip_address(std::string_view value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
        value.remove_prefix(1);
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
        value.remove_suffix(1);
    }
    if (value.empty()) {
        return false;
    }
    // IPv4: a.b.c.d with each octet 0–255.
    if (value.find(':') == std::string_view::npos) {
        int octets = 0;
        int current = 0;
        int digits = 0;
        bool in_octet = false;
        for (char ch : value) {
            if (ch == '.') {
                if (!in_octet || digits == 0 || current > 255) {
                    return false;
                }
                ++octets;
                current = 0;
                digits = 0;
                in_octet = false;
                continue;
            }
            if (ch < '0' || ch > '9') {
                return false;
            }
            in_octet = true;
            current = current * 10 + (ch - '0');
            ++digits;
            if (digits > 3 || current > 255) {
                return false;
            }
        }
        return in_octet && digits > 0 && current <= 255 && octets == 3;
    }
    // IPv6: require at least two hex groups separated by ':' (loose visual check).
    int colons = 0;
    int hex_run = 0;
    bool saw_hex = false;
    for (char ch : value) {
        if (ch == ':') {
            ++colons;
            hex_run = 0;
            continue;
        }
        if (ch == '.') {
            // IPv4-mapped IPv6 (::ffff:1.2.3.4) — allow dots after we already saw ':'.
            continue;
        }
        const auto lower = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if ((lower >= '0' && lower <= '9') || (lower >= 'a' && lower <= 'f')) {
            ++hex_run;
            saw_hex = true;
            if (hex_run > 4) {
                return false;
            }
            continue;
        }
        return false;
    }
    return saw_hex && colons >= 2;
}

/**
 * Ordered connect targets: primary, then alt when set/valid/different.
 * Invalid alt is omitted (caller may warn separately).
 */
inline std::vector<std::string> host_connect_candidates(
    std::string_view primary,
    std::string_view alt) {
    auto trim = [](std::string_view v) {
        while (!v.empty() && std::isspace(static_cast<unsigned char>(v.front()))) {
            v.remove_prefix(1);
        }
        while (!v.empty() && std::isspace(static_cast<unsigned char>(v.back()))) {
            v.remove_suffix(1);
        }
        return v;
    };
    const auto first = trim(primary);
    const auto second = trim(alt);
    std::vector<std::string> out;
    if (!first.empty()) {
        out.emplace_back(first);
    }
    if (!second.empty() && looks_like_ip_address(second)) {
        const bool same = !out.empty() &&
            out.front().size() == second.size() &&
            std::equal(
                out.front().begin(),
                out.front().end(),
                second.begin(),
                [](char a, char b) {
                    return std::tolower(static_cast<unsigned char>(a)) ==
                        std::tolower(static_cast<unsigned char>(b));
                });
        if (!same) {
            out.emplace_back(second);
        }
    }
    return out;
}

inline bool is_tcp_reachability_failure_message(std::string_view message) {
    // Matches common asio / system_error text from TcpStream::connect_to.
    std::string lower;
    lower.reserve(message.size());
    for (char ch : message) {
        lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    return lower.find("failed to connect") != std::string::npos ||
        lower.find("connection refused") != std::string::npos ||
        lower.find("timed out") != std::string::npos ||
        lower.find("timeout") != std::string::npos ||
        lower.find("network is unreachable") != std::string::npos ||
        lower.find("no route to host") != std::string::npos ||
        lower.find("host is unreachable") != std::string::npos ||
        lower.find("name or service not known") != std::string::npos;
}

} // namespace archstreamer
