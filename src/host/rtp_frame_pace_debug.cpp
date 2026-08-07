// TEMP: frame pacing debug — remove when judder investigation is done.

#include "host/rtp_frame_pace_debug.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace archstreamer::rtp_frame_pace_debug {
namespace {

constexpr std::uint16_t kSniffPortBase = 39100;

struct TeeState {
    std::uint16_t encode_port = 0;
    std::uint16_t sniff_port = 0;
    std::atomic<bool> running{false};
    std::thread thread;
};

std::mutex g_mu;
std::unordered_map<std::uint16_t, std::unique_ptr<TeeState>> g_tees;

void sniffer_main(TeeState* state) {
    const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        std::cerr << "pace host sniff socket failed for encode_port=" << state->encode_port << '\n';
        return;
    }
    int yes = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(state->sniff_port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        std::cerr << "pace host sniff bind failed port=" << state->sniff_port << '\n';
        ::close(fd);
        return;
    }
    timeval tv{};
    tv.tv_sec = 0;
    tv.tv_usec = 200000;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    using clock = std::chrono::steady_clock;
    auto window_start = clock::now();
    std::optional<clock::time_point> last_marker;
    std::vector<double> dts_ms;
    dts_ms.reserve(64);
    std::uint8_t buf[2048];

    auto flush = [&](bool force) {
        const auto now = clock::now();
        if (!force && now - window_start < std::chrono::seconds(1)) {
            return;
        }
        if (dts_ms.empty()) {
            window_start = now;
            return;
        }
        std::sort(dts_ms.begin(), dts_ms.end());
        const auto n = dts_ms.size();
        const double p50 = dts_ms[n / 2];
        const double p95 = dts_ms[(n * 95) / 100];
        const double max_dt = dts_ms.back();
        double sum = 0;
        for (double v : dts_ms) {
            sum += v;
        }
        std::cout
            << "pace host encode_port=" << state->encode_port
            << " sniff=" << state->sniff_port
            << " n=" << n
            << " dt_ms avg=" << (sum / static_cast<double>(n))
            << " p50=" << p50
            << " p95=" << p95
            << " max=" << max_dt
            << '\n';
        dts_ms.clear();
        window_start = now;
    };

    while (state->running.load(std::memory_order_relaxed)) {
        const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n < 12) {
            flush(false);
            continue;
        }
        const std::uint8_t v = static_cast<std::uint8_t>((buf[0] >> 6) & 0x3);
        if (v != 2) {
            continue;
        }
        const bool marker = (buf[1] & 0x80) != 0;
        if (!marker) {
            continue;
        }
        const auto now = clock::now();
        if (last_marker.has_value()) {
            const double dt = std::chrono::duration<double, std::milli>(now - *last_marker).count();
            if (dt > 0.0 && dt < 1000.0) {
                dts_ms.push_back(dt);
            }
        }
        last_marker = now;
        flush(false);
    }
    flush(true);
    ::close(fd);
}

} // namespace

bool enabled() {
    const char* v = std::getenv("ARCHSTREAMER_DEBUG_FRAME_PACE");
    return v != nullptr && v[0] != '\0' && std::strcmp(v, "0") != 0;
}

std::optional<std::pair<std::string, std::uint16_t>> ensure_tee(std::uint16_t encode_port) {
    if (!enabled() || encode_port == 0) {
        return std::nullopt;
    }
    std::lock_guard lock(g_mu);
    auto it = g_tees.find(encode_port);
    if (it != g_tees.end() && it->second && it->second->running.load()) {
        return std::make_pair(std::string{"127.0.0.1"}, it->second->sniff_port);
    }
    auto state = std::make_unique<TeeState>();
    state->encode_port = encode_port;
    state->sniff_port = static_cast<std::uint16_t>(kSniffPortBase + (encode_port % 500));
    state->running = true;
    TeeState* raw = state.get();
    state->thread = std::thread(sniffer_main, raw);
    std::cout
        << "pace host tee enabled encode_port=" << encode_port
        << " sniff=127.0.0.1:" << state->sniff_port
        << " (ARCHSTREAMER_DEBUG_FRAME_PACE)\n";
    const auto sniff = state->sniff_port;
    g_tees[encode_port] = std::move(state);
    return std::make_pair(std::string{"127.0.0.1"}, sniff);
}

void stop_tee(std::uint16_t encode_port) {
    std::unique_ptr<TeeState> state;
    {
        std::lock_guard lock(g_mu);
        auto it = g_tees.find(encode_port);
        if (it == g_tees.end()) {
            return;
        }
        state = std::move(it->second);
        g_tees.erase(it);
    }
    if (!state) {
        return;
    }
    state->running = false;
    if (state->thread.joinable()) {
        state->thread.join();
    }
}

void stop_all() {
    std::vector<std::uint16_t> ports;
    {
        std::lock_guard lock(g_mu);
        ports.reserve(g_tees.size());
        for (const auto& [port, _] : g_tees) {
            ports.push_back(port);
        }
    }
    for (const auto port : ports) {
        stop_tee(port);
    }
}

} // namespace archstreamer::rtp_frame_pace_debug
