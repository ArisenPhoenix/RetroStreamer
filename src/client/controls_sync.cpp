#include "client/controls_sync.hpp"

#include "client/catalog_blocks_cache.hpp"
#include "client/session_service.hpp"
#include "common/client_debug_log.hpp"
#include "common/protocol.hpp"
#include "common/serialization.hpp"

#include <stdexcept>
#include <utility>

namespace archstreamer {
namespace {

ControlsSyncResult fail(std::string message) {
    ControlsSyncResult result;
    result.ok = false;
    result.message = std::move(message);
    return result;
}

TcpStream connect_and_authenticate(
    const std::string& host,
    std::uint16_t control_port,
    const std::string& username,
    const std::string& password) {
    if (username.empty()) {
        throw std::runtime_error("profile username is required");
    }

    client_debug_log_conn(
        "TCP connect begin (controls sync) " + host + ":" + std::to_string(control_port));
    auto stream = TcpStream::connect_to(host, control_port);
    client_debug_log_conn("TCP connected (controls sync)");
    // Host accepts LobbyPresence after the catalog exchange on this socket.
    stream.send_packet(serialize_packet(GameListRequest{0}));
    const auto catalog_payload = receive_client_control_payload(stream);
    if (std::get_if<GameList>(&catalog_payload) == nullptr) {
        if (const auto* error = std::get_if<ErrorPacket>(&catalog_payload); error != nullptr) {
            throw std::runtime_error(error->message.empty() ? "catalog failed" : error->message);
        }
        throw std::runtime_error("expected GameList from host");
    }

    LobbyPresence presence;
    presence.username = username;
    presence.password = password;
    try {
        const auto blocks_cache = load_catalog_blocks_cache(
            default_catalog_blocks_cache_path(), host, control_port, username);
        presence.client_blocks_revision = blocks_cache.blocks_revision;
    } catch (const std::exception&) {
    }
    stream.send_packet(serialize_packet(presence));
    while (true) {
        const auto ack_payload = receive_client_control_payload(stream);
        if (std::get_if<CatalogUserBlocks>(&ack_payload) != nullptr) {
            // Per-user blocks after auth — ignore for controls sync.
            continue;
        }
        if (std::get_if<GameList>(&ack_payload) != nullptr) {
            // Legacy filtered catalog after auth — ignore for controls sync.
            continue;
        }
        if (std::get_if<LobbyPresenceAck>(&ack_payload) != nullptr) {
            break;
        }
        if (const auto* error = std::get_if<ErrorPacket>(&ack_payload); error != nullptr) {
            throw std::runtime_error(
                error->message.empty() ? "lobby presence failed" : error->message);
        }
        throw std::runtime_error("expected LobbyPresenceAck from host");
    }
    return stream;
}

} // namespace

ControlsSyncResult pull_controls_db_from_host(
    const std::string& host,
    std::uint16_t control_port,
    const std::string& username,
    const std::string& password) {
    try {
        auto stream = connect_and_authenticate(host, control_port, username, password);
        ControlsDbPull pull;
        pull.username = username;
        stream.send_packet(serialize_packet(pull));
        const auto payload = receive_client_control_payload(stream);
        if (const auto* response = std::get_if<ControlsDbResponse>(&payload);
            response != nullptr) {
            ControlsSyncResult result;
            result.ok = true;
            result.found = response->found && !response->db_bytes.empty();
            result.db_bytes = response->db_bytes;
            result.message = result.found
                ? ("Pulled controls for " + username)
                : ("No controls stored on host for " + username);
            return result;
        }
        if (const auto* error = std::get_if<ErrorPacket>(&payload); error != nullptr) {
            return fail(error->message.empty() ? "pull rejected" : error->message);
        }
        return fail("unexpected response to ControlsDbPull");
    } catch (const std::exception& ex) {
        return fail(ex.what());
    }
}

ControlsSyncResult push_controls_db_to_host(
    const std::string& host,
    std::uint16_t control_port,
    const std::string& username,
    const std::string& password,
    const std::vector<std::uint8_t>& db_bytes) {
    try {
        if (db_bytes.empty()) {
            return fail("nothing to push (empty controls pack)");
        }
        auto stream = connect_and_authenticate(host, control_port, username, password);
        ControlsDbPush push;
        push.username = username;
        push.db_bytes = db_bytes;
        stream.send_packet(serialize_packet(push));
        const auto payload = receive_client_control_payload(stream);
        if (const auto* ack = std::get_if<ControlsDbAck>(&payload); ack != nullptr) {
            ControlsSyncResult result;
            result.ok = ack->ok;
            result.message = ack->ok
                ? ("Pushed controls for " + username)
                : (ack->message.empty() ? "push rejected" : ack->message);
            return result;
        }
        if (const auto* error = std::get_if<ErrorPacket>(&payload); error != nullptr) {
            return fail(error->message.empty() ? "push rejected" : error->message);
        }
        return fail("unexpected response to ControlsDbPush");
    } catch (const std::exception& ex) {
        return fail(ex.what());
    }
}

} // namespace archstreamer
