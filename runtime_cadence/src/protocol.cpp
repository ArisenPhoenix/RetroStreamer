#include "archstreamer/runtime_cadence/protocol.hpp"

#include <cstring>

#if !defined(_WIN32)
#include <unistd.h>
#endif

namespace archstreamer::cadence {
namespace {

bool read_exact(int fd, void* buf, std::size_t n) {
#if defined(_WIN32)
    (void)fd;
    (void)buf;
    (void)n;
    return false;
#else
    auto* p = static_cast<char*>(buf);
    std::size_t got = 0;
    while (got < n) {
        const auto r = ::read(fd, p + got, n - got);
        if (r <= 0) {
            return false;
        }
        got += static_cast<std::size_t>(r);
    }
    return true;
#endif
}

bool write_exact(int fd, const void* buf, std::size_t n) {
#if defined(_WIN32)
    (void)fd;
    (void)buf;
    (void)n;
    return false;
#else
    auto* p = static_cast<const char*>(buf);
    std::size_t sent = 0;
    while (sent < n) {
        const auto w = ::write(fd, p + sent, n - sent);
        if (w <= 0) {
            return false;
        }
        sent += static_cast<std::size_t>(w);
    }
    return true;
#endif
}

std::uint32_t read_be32(const unsigned char* p) {
    return (std::uint32_t{p[0]} << 24) | (std::uint32_t{p[1]} << 16) |
        (std::uint32_t{p[2]} << 8) | std::uint32_t{p[3]};
}

void write_be32(unsigned char* p, std::uint32_t v) {
    p[0] = static_cast<unsigned char>((v >> 24) & 0xff);
    p[1] = static_cast<unsigned char>((v >> 16) & 0xff);
    p[2] = static_cast<unsigned char>((v >> 8) & 0xff);
    p[3] = static_cast<unsigned char>(v & 0xff);
}

} // namespace

nlohmann::json user_to_json(const UserRecord& user) {
    return {
        {"username", user.username},
        {"display_name", user.display_name},
        {"password_hash", user.password_hash},
        {"must_change", user.must_change},
        {"created_at", user.created_at},
        {"updated_at", user.updated_at},
    };
}

UserRecord user_from_json(const nlohmann::json& j) {
    UserRecord user;
    user.username = j.value("username", "");
    user.display_name = j.value("display_name", "");
    user.password_hash = j.value("password_hash", "");
    // Accept accidental "password" key from older tooling as legacy plaintext.
    if (user.password_hash.empty() && j.contains("password")) {
        user.password_hash = j.value("password", "");
    }
    user.must_change = j.value("must_change", false);
    user.created_at = j.value("created_at", std::int64_t{0});
    user.updated_at = j.value("updated_at", std::int64_t{0});
    return user;
}

nlohmann::json event_to_json(const RuntimeEvent& event) {
    return {
        {"timestamp", event.timestamp},
        {"kind", event.kind},
        {"host_id", event.host_id},
        {"slot", event.slot},
        {"username", event.username},
        {"game_key", event.game_key},
        {"detail", event.detail},
        {"session_id", event.session_id},
    };
}

RuntimeEvent event_from_json(const nlohmann::json& j) {
    RuntimeEvent event;
    event.timestamp = j.value("timestamp", std::int64_t{0});
    event.kind = j.value("kind", "");
    event.host_id = j.value("host_id", "");
    event.slot = j.value("slot", -1);
    event.username = j.value("username", "");
    event.game_key = j.value("game_key", "");
    event.detail = j.value("detail", "");
    event.session_id = j.value("session_id", "");
    return event;
}

nlohmann::json session_to_json(const SessionRecord& session) {
    return {
        {"session_id", session.session_id},
        {"host_id", session.host_id},
        {"slot", session.slot},
        {"username", session.username},
        {"game_key", session.game_key},
        {"system_key", session.system_key},
        {"mode", session.mode},
        {"started_at", session.started_at},
        {"ended_at", session.ended_at},
        {"end_reason", session.end_reason},
    };
}

SessionRecord session_from_json(const nlohmann::json& j) {
    SessionRecord session;
    session.session_id = j.value("session_id", "");
    session.host_id = j.value("host_id", "");
    session.slot = j.value("slot", -1);
    session.username = j.value("username", "");
    session.game_key = j.value("game_key", "");
    session.system_key = j.value("system_key", "");
    session.mode = j.value("mode", "");
    session.started_at = j.value("started_at", std::int64_t{0});
    session.ended_at = j.value("ended_at", std::int64_t{0});
    session.end_reason = j.value("end_reason", "");
    return session;
}

nlohmann::json claim_to_json(const ResourceClaim& claim) {
    return {
        {"session_id", claim.session_id},
        {"resource_type", claim.resource_type},
        {"resource_name", claim.resource_name},
        {"host_id", claim.host_id},
        {"slot", claim.slot},
        {"claimed_at", claim.claimed_at},
        {"released_at", claim.released_at},
        {"detail", claim.detail},
    };
}

ResourceClaim claim_from_json(const nlohmann::json& j) {
    ResourceClaim claim;
    claim.session_id = j.value("session_id", "");
    claim.resource_type = j.value("resource_type", "");
    claim.resource_name = j.value("resource_name", "");
    claim.host_id = j.value("host_id", "");
    claim.slot = j.value("slot", -1);
    claim.claimed_at = j.value("claimed_at", std::int64_t{0});
    claim.released_at = j.value("released_at", std::int64_t{0});
    claim.detail = j.value("detail", "");
    return claim;
}

nlohmann::json make_request(const std::string& op, nlohmann::json fields) {
    if (!fields.is_object()) {
        fields = nlohmann::json::object();
    }
    fields["op"] = op;
    return fields;
}

bool read_frame(int fd, std::string& out_payload) {
    unsigned char hdr[4];
    if (!read_exact(fd, hdr, 4)) {
        return false;
    }
    const auto len = read_be32(hdr);
    if (len == 0 || len > kMaxProtocolPayload) {
        return false;
    }
    out_payload.assign(len, '\0');
    return read_exact(fd, out_payload.data(), len);
}

bool write_frame(int fd, const std::string& payload) {
    if (payload.size() > kMaxProtocolPayload) {
        return false;
    }
    unsigned char hdr[4];
    write_be32(hdr, static_cast<std::uint32_t>(payload.size()));
    return write_exact(fd, hdr, 4) && write_exact(fd, payload.data(), payload.size());
}

} // namespace archstreamer::cadence
