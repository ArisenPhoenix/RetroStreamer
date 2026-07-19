#include "client/session_service.hpp"

#include "client/catalog_cache.hpp"
#include "common/serialization.hpp"

#include <stdexcept>
#include <utility>

namespace archstreamer {

PacketPayload receive_client_control_payload(TcpStream& stream) {
    const auto packet = stream.receive_packet();
    if (!packet.has_value()) {
        throw std::runtime_error("host disconnected");
    }

    return deserialize_packet(*packet);
}

ClientSessionService::ClientSessionService(std::string host, std::uint16_t control_port)
    : ClientSessionService(std::move(host), control_port, default_catalog_cache_path()) {
}

ClientSessionService::ClientSessionService(
    std::string host,
    std::uint16_t control_port,
    std::filesystem::path catalog_cache_path)
    : host_(std::move(host)), control_port_(control_port), catalog_cache_path_(std::move(catalog_cache_path)) {
}

PendingSession ClientSessionService::begin() const {
    auto stream = TcpStream::connect_to(host_, control_port_);
    SessionClient session;
    auto cached_game_list = load_catalog_cache(catalog_cache_path_);
    stream.send_packet(serialize_packet(session.make_game_list_request(cached_game_list.catalog_revision)));

    const auto payload = receive_client_control_payload(stream);
    const auto* game_list = std::get_if<GameList>(&payload);
    if (game_list == nullptr) {
        throw std::runtime_error("expected GameList from host");
    }
    merge_catalog_delta(cached_game_list, *game_list);
    save_catalog_cache(catalog_cache_path_, cached_game_list);
    session.apply_game_list(cached_game_list);

    return PendingSession{
        std::move(session),
        std::move(stream),
        std::move(cached_game_list),
    };
}

ActiveSessionInfo ClientSessionService::active_session_info() const {
    auto stream = TcpStream::connect_to(host_, control_port_);
    stream.send_packet(serialize_packet(ActiveSessionInfoRequest{}));

    const auto payload = receive_client_control_payload(stream);
    const auto* info = std::get_if<ActiveSessionInfo>(&payload);
    if (info == nullptr) {
        throw std::runtime_error("expected ActiveSessionInfo from host");
    }
    return *info;
}

JoinedSession ClientSessionService::finish_join(PendingSession pending, const ClientHello& hello) const {
    pending.stream.send_packet(serialize_packet(hello));

    const auto welcome_payload = receive_client_control_payload(pending.stream);
    if (const auto* error = std::get_if<ErrorPacket>(&welcome_payload); error != nullptr) {
        throw std::runtime_error("host rejected session: " + error->message);
    }
    const auto* welcome = std::get_if<HostWelcome>(&welcome_payload);
    if (welcome == nullptr) {
        throw std::runtime_error("expected HostWelcome from host");
    }
    pending.session.apply_welcome(*welcome);

    const auto seats_payload = receive_client_control_payload(pending.stream);
    const auto* seats = std::get_if<SeatAssignment>(&seats_payload);
    if (seats == nullptr) {
        throw std::runtime_error("expected SeatAssignment from host");
    }
    pending.session.apply_seats(*seats);

    const auto ready_payload = receive_client_control_payload(pending.stream);
    const auto* ready = std::get_if<SessionReady>(&ready_payload);
    if (ready == nullptr) {
        throw std::runtime_error("expected SessionReady from host");
    }

    return JoinedSession{
        std::move(pending.session),
        std::move(pending.stream),
        std::move(pending.game_list),
        *ready,
    };
}

SessionStart ClientSessionService::wait_for_starting(TcpStream& stream) const {
    auto media_endpoint = std::optional<MediaEndpoint>{};
    while (true) {
        const auto payload = receive_client_control_payload(stream);
        if (const auto* error = std::get_if<ErrorPacket>(&payload); error != nullptr) {
            throw std::runtime_error("host rejected session: " + error->message);
        }
        if (const auto* endpoint = std::get_if<MediaEndpoint>(&payload); endpoint != nullptr) {
            media_endpoint = *endpoint;
            continue;
        }
        const auto* starting = std::get_if<SessionStarting>(&payload);
        if (starting == nullptr) {
            throw std::runtime_error("expected MediaEndpoint or SessionStarting from host");
        }

        return SessionStart{
            *starting,
            std::move(media_endpoint),
        };
    }
}

} // namespace archstreamer
