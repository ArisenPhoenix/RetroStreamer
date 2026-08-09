#include "common/pairing.hpp"

#include <charconv>
#include <iomanip>
#include <map>
#include <sstream>
#include <string_view>

namespace archstreamer {
namespace {

bool is_unreserved(char ch) {
    return (ch >= 'a' && ch <= 'z') ||
        (ch >= 'A' && ch <= 'Z') ||
        (ch >= '0' && ch <= '9') ||
        ch == '-' || ch == '_' || ch == '.' || ch == '~';
}

std::string url_encode(std::string_view value) {
    std::ostringstream out;
    out << std::uppercase << std::hex;
    for (const unsigned char ch : value) {
        if (is_unreserved(static_cast<char>(ch))) {
            out << static_cast<char>(ch);
        } else {
            out << '%' << std::setw(2) << std::setfill('0') << static_cast<int>(ch);
        }
    }
    return out.str();
}

int hex_value(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return 10 + ch - 'a';
    if (ch >= 'A' && ch <= 'F') return 10 + ch - 'A';
    return -1;
}

std::string url_decode(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        const char ch = value[i];
        if (ch == '%' && i + 2 < value.size()) {
            const int hi = hex_value(value[i + 1]);
            const int lo = hex_value(value[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        out.push_back(ch == '+' ? ' ' : ch);
    }
    return out;
}

std::optional<std::uint16_t> parse_port(const std::string& value) {
    int port = 0;
    const auto* begin = value.data();
    const auto* end = begin + value.size();
    const auto [ptr, ec] = std::from_chars(begin, end, port);
    if (ec != std::errc{} || ptr != end || port < 1 || port > 65535) {
        return std::nullopt;
    }
    return static_cast<std::uint16_t>(port);
}

void set_error(std::string* error, const char* message) {
    if (error != nullptr) {
        *error = message;
    }
}

} // namespace

std::string build_pair_uri(
    const std::string& ip,
    std::uint16_t port,
    const std::string& token,
    const std::string& relay_host,
    std::uint16_t relay_port) {
    std::string uri = "archstreamer://pair?v=1&ip=" + url_encode(ip) +
        "&port=" + std::to_string(port) +
        "&token=" + url_encode(token);
    if (!relay_host.empty() && relay_port != 0) {
        uri += "&rh=" + url_encode(relay_host) + "&rp=" + std::to_string(relay_port);
    }
    return uri;
}

std::optional<PairTarget> parse_pair_uri(std::string_view uri, std::string* error) {
    constexpr std::string_view prefix = "archstreamer://pair";
    if (uri.substr(0, prefix.size()) != prefix) {
        set_error(error, "Not an ArchStreamer pair QR");
        return std::nullopt;
    }
    const auto query_pos = uri.find('?');
    if (query_pos == std::string_view::npos || query_pos + 1 >= uri.size()) {
        set_error(error, "Pair QR missing query");
        return std::nullopt;
    }

    std::map<std::string, std::string> params;
    std::string_view query = uri.substr(query_pos + 1);
    while (!query.empty()) {
        const auto amp = query.find('&');
        const auto part = query.substr(0, amp);
        if (!part.empty()) {
            const auto eq = part.find('=');
            if (eq != std::string_view::npos && eq > 0) {
                params[url_decode(part.substr(0, eq))] = url_decode(part.substr(eq + 1));
            }
        }
        if (amp == std::string_view::npos) {
            break;
        }
        query.remove_prefix(amp + 1);
    }

    PairTarget target;
    target.ip = params["ip"];
    target.token = params["token"];
    const auto port = parse_port(params["port"]);
    if (target.ip.empty() || target.token.empty() || !port.has_value()) {
        set_error(error, "Pair QR missing ip/port/token");
        return std::nullopt;
    }
    target.port = *port;
    target.relay_host = params["rh"];
    if (const auto relay_port = parse_port(params["rp"]); relay_port.has_value()) {
        target.relay_port = *relay_port;
    }
    return target;
}

} // namespace archstreamer
