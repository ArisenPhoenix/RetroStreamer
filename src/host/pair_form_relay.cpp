#include "host/pair_form_relay.hpp"

#include "common/serialization.hpp"

#include <chrono>
#include <mutex>
#include <unordered_map>
#include <variant>

namespace archstreamer {
namespace {

constexpr auto kRelayTtl = std::chrono::minutes(2);
constexpr std::size_t kMaxRelayBytes = 64u * 1024u;

struct RelayEntry {
    std::vector<std::uint8_t> profile_json;
    std::chrono::steady_clock::time_point expires_at;
};

std::mutex g_mutex;
std::unordered_map<std::string, RelayEntry> g_entries;

bool valid_token(std::string_view token) {
    if (token.size() < 12 || token.size() > 128) {
        return false;
    }
    for (const char ch : token) {
        const bool alpha = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
        const bool digit = ch >= '0' && ch <= '9';
        if (!alpha && !digit && ch != '-' && ch != '_') {
            return false;
        }
    }
    return true;
}

void reap_expired_locked(std::chrono::steady_clock::time_point now) {
    for (auto it = g_entries.begin(); it != g_entries.end();) {
        if (it->second.expires_at <= now) {
            it = g_entries.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace

PairFormRelayAck handle_pair_form_relay_push(const PairFormRelayPush& push) {
    if (!valid_token(push.token)) {
        return PairFormRelayAck{false, "invalid pair token"};
    }
    if (push.profile_json.empty()) {
        return PairFormRelayAck{false, "empty form bundle"};
    }
    if (push.profile_json.size() > kMaxRelayBytes) {
        return PairFormRelayAck{false, "form bundle too large"};
    }

    const auto now = std::chrono::steady_clock::now();
    std::lock_guard lock(g_mutex);
    reap_expired_locked(now);
    g_entries[push.token] = RelayEntry{push.profile_json, now + kRelayTtl};
    return PairFormRelayAck{true, "forms relayed through host"};
}

PairFormRelayResponse handle_pair_form_relay_pull(const PairFormRelayPull& pull) {
    if (!valid_token(pull.token)) {
        return PairFormRelayResponse{false, {}, "invalid pair token"};
    }

    const auto now = std::chrono::steady_clock::now();
    std::lock_guard lock(g_mutex);
    reap_expired_locked(now);
    auto it = g_entries.find(pull.token);
    if (it == g_entries.end()) {
        return PairFormRelayResponse{false, {}, "no relayed forms for token"};
    }
    auto bytes = std::move(it->second.profile_json);
    g_entries.erase(it);
    return PairFormRelayResponse{true, std::move(bytes), "forms received through host"};
}

bool is_pair_form_relay_packet(const PacketPayload& payload) {
    return std::holds_alternative<PairFormRelayPush>(payload) ||
        std::holds_alternative<PairFormRelayPull>(payload);
}

ByteBuffer handle_pair_form_relay_packet(const PacketPayload& payload) {
    if (const auto* push = std::get_if<PairFormRelayPush>(&payload); push != nullptr) {
        return serialize_packet(handle_pair_form_relay_push(*push));
    }
    if (const auto* pull = std::get_if<PairFormRelayPull>(&payload); pull != nullptr) {
        return serialize_packet(handle_pair_form_relay_pull(*pull));
    }
    return {};
}

} // namespace archstreamer
