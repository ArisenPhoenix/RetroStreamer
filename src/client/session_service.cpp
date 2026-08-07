#include "client/session_service.hpp"

#include "client/catalog_blocks_cache.hpp"
#include "client/catalog_cache.hpp"
#include "common/art_transfer.hpp"
#include "common/client_debug_log.hpp"
#include "common/serialization.hpp"

#include <algorithm>
#include <fstream>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace archstreamer {

PacketPayload receive_client_control_payload(TcpStream& stream) {
    const auto packet = stream.receive_packet();
    if (!packet.has_value()) {
        throw std::runtime_error("host disconnected");
    }

    return deserialize_packet(*packet);
}

namespace {

void apply_blocks_to_pending(GameList& list, const std::vector<GameId>& blocked_game_ids) {
    if (blocked_game_ids.empty() || list.games.empty()) {
        return;
    }
    std::unordered_set<std::string> blocked(blocked_game_ids.begin(), blocked_game_ids.end());
    list.games.erase(
        std::remove_if(
            list.games.begin(),
            list.games.end(),
            [&](const GameInfo& game) { return blocked.contains(game.id); }),
        list.games.end());
}

// Picker-facing roles only — full kDisplayArtRoles is overkill for catalog sync.
constexpr std::string_view kCatalogArtRoles[] = {
    "boxart",
    "grid",
    "icon",
};

std::uint64_t read_art_sync_revision(const std::filesystem::path& art_cache_root) {
    std::ifstream file(art_cache_root / ".art_sync_catalog_revision");
    if (!file) {
        return 0;
    }
    std::uint64_t revision = 0;
    file >> revision;
    return revision;
}

void write_art_sync_revision(
    const std::filesystem::path& art_cache_root,
    std::uint64_t catalog_revision) {
    std::ofstream file(art_cache_root / ".art_sync_catalog_revision", std::ios::trunc);
    if (file) {
        file << catalog_revision << '\n';
    }
}

// Art files live under art_cache_root (same layout as host Art/). Sync is gated on the
// catalog offerings revision — unchanged catalog → zero art TCP. When needed, one
// control connection does GameList then all missing ArtAssetRequests (host accepts
// ArtAsset after GameList on the same socket).
void sync_art_for_catalog(
    const std::string& host,
    std::uint16_t control_port,
    const GameList& catalog,
    const std::filesystem::path& art_cache_root) {
    std::filesystem::create_directories(art_cache_root);
    if (catalog.catalog_revision != 0 &&
        read_art_sync_revision(art_cache_root) == catalog.catalog_revision) {
        return;
    }

    bool need_fetch = false;
    for (const auto& game : catalog.games) {
        if (game.asset_key.empty()) {
            continue;
        }
        for (const auto role : kCatalogArtRoles) {
            if (!art_asset_exists_locally(art_cache_root, game.asset_key, role)) {
                need_fetch = true;
                break;
            }
        }
        if (need_fetch) {
            break;
        }
    }
    if (!need_fetch) {
        write_art_sync_revision(art_cache_root, catalog.catalog_revision);
        return;
    }

    auto stream = TcpStream::connect_to(host, control_port);
    stream.send_packet(serialize_packet(GameListRequest{catalog.catalog_revision}));
    (void)receive_client_control_payload(stream);

    for (const auto& game : catalog.games) {
        if (game.asset_key.empty()) {
            continue;
        }
        for (const auto role : kCatalogArtRoles) {
            if (art_asset_exists_locally(art_cache_root, game.asset_key, role)) {
                continue;
            }
            ArtAssetRequest request{
                game.asset_key,
                std::string(role),
                {},
            };
            stream.send_packet(serialize_packet(request));
            const auto payload = receive_client_control_payload(stream);
            const auto* response = std::get_if<ArtAssetResponse>(&payload);
            if (response == nullptr) {
                throw std::runtime_error("expected ArtAssetResponse from host");
            }
            write_art_asset_to_cache(art_cache_root, *response);
        }
    }
    write_art_sync_revision(art_cache_root, catalog.catalog_revision);
}

} // namespace

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
    client_debug_log_conn(
        "TCP connect begin " + host_ + ":" + std::to_string(control_port_));
    auto stream = TcpStream::connect_to(host_, control_port_);
    client_debug_log_conn(
        "TCP connected " + host_ + ":" + std::to_string(control_port_));
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
        host_art_cache_root(sanitize_host_cache_id("host", host_)),
    };
}

void ClientSessionService::sync_catalog_art(const PendingSession& pending) const {
    try {
        sync_art_for_catalog(host_, control_port_, pending.game_list, pending.art_cache_root);
    } catch (const std::exception&) {
        // Older hosts without art protocol still return a usable catalog.
    }
}

void ClientSessionService::apply_authenticated_catalog_filter(
    PendingSession& pending,
    std::string_view username,
    std::string_view password) const {
    if (!valid_username(username) || password.empty()) {
        return;
    }
    const auto blocks_path = default_catalog_blocks_cache_path();
    auto blocks_cache = load_catalog_blocks_cache(
        blocks_path, host_, control_port_, username);

    LobbyPresence presence;
    presence.username = std::string(username);
    presence.password = std::string(password);
    presence.client_blocks_revision = blocks_cache.blocks_revision;
    pending.stream.send_packet(serialize_packet(presence));
    while (true) {
        const auto payload = receive_client_control_payload(pending.stream);
        if (const auto* blocks = std::get_if<CatalogUserBlocks>(&payload); blocks != nullptr) {
            merge_catalog_blocks_cache(blocks_cache, *blocks);
            try {
                save_catalog_blocks_cache(
                    blocks_path, host_, control_port_, username, blocks_cache);
            } catch (const std::exception&) {
            }
            pending.blocks_revision = blocks_cache.blocks_revision;
            pending.blocked_game_ids = blocks_cache.blocked_game_ids;
            apply_blocks_to_pending(pending.game_list, pending.blocked_game_ids);
            pending.session.apply_game_list(pending.game_list);
            continue;
        }
        if (const auto* catalog = std::get_if<GameList>(&payload); catalog != nullptr) {
            // Legacy hosts resent a filtered catalog after auth.
            pending.game_list = *catalog;
            pending.session.apply_game_list(pending.game_list);
            continue;
        }
        if (std::get_if<LobbyPresenceAck>(&payload) != nullptr) {
            return;
        }
        if (const auto* error = std::get_if<ErrorPacket>(&payload); error != nullptr) {
            throw std::runtime_error(
                error->message.empty() ? "lobby presence failed" : error->message);
        }
        throw std::runtime_error("expected LobbyPresenceAck from host");
    }
}

ActiveSessionInfo ClientSessionService::active_session_info() const {
    client_debug_log_conn(
        "TCP connect begin (ActiveSessionInfo) " + host_ + ":" +
        std::to_string(control_port_));
    auto stream = TcpStream::connect_to(host_, control_port_);
    client_debug_log_conn("TCP connected (ActiveSessionInfo)");
    stream.send_packet(serialize_packet(ActiveSessionInfoRequest{}));

    const auto payload = receive_client_control_payload(stream);
    const auto* info = std::get_if<ActiveSessionInfo>(&payload);
    if (info == nullptr) {
        throw std::runtime_error("expected ActiveSessionInfo from host");
    }
    return *info;
}

JoinedSession ClientSessionService::finish_join(
    PendingSession pending,
    ClientHello hello,
    const std::function<std::string(const std::string& current_password)>&
        on_password_change_required) const {
    const auto blocks_path = default_catalog_blocks_cache_path();
    auto blocks_cache = load_catalog_blocks_cache(
        blocks_path, host_, control_port_, hello.username);
    if (hello.client_blocks_revision == 0) {
        hello.client_blocks_revision = blocks_cache.blocks_revision;
    }
    if (pending.blocked_game_ids.empty()) {
        pending.blocked_game_ids = blocks_cache.blocked_game_ids;
        pending.blocks_revision = blocks_cache.blocks_revision;
    }
    pending.stream.send_packet(serialize_packet(hello));

    auto welcome_payload = receive_client_control_payload(pending.stream);
    while (true) {
        if (const auto* error = std::get_if<ErrorPacket>(&welcome_payload); error != nullptr) {
            throw std::runtime_error("host rejected session: " + error->message);
        }
        if (const auto* blocks = std::get_if<CatalogUserBlocks>(&welcome_payload);
            blocks != nullptr) {
            merge_catalog_blocks_cache(blocks_cache, *blocks);
            try {
                save_catalog_blocks_cache(
                    blocks_path, host_, control_port_, hello.username, blocks_cache);
            } catch (const std::exception&) {
            }
            pending.blocks_revision = blocks_cache.blocks_revision;
            pending.blocked_game_ids = blocks_cache.blocked_game_ids;
            apply_blocks_to_pending(pending.game_list, pending.blocked_game_ids);
            welcome_payload = receive_client_control_payload(pending.stream);
            continue;
        }
        if (const auto* catalog = std::get_if<GameList>(&welcome_payload); catalog != nullptr) {
            // Legacy: host resent a user-filtered catalog after auth.
            pending.game_list = *catalog;
            welcome_payload = receive_client_control_payload(pending.stream);
            continue;
        }
        if (std::holds_alternative<PasswordChangeRequired>(welcome_payload)) {
            if (!on_password_change_required) {
                throw std::runtime_error("host requires a password change");
            }
            const auto new_password = on_password_change_required(hello.password);
            if (new_password.empty()) {
                throw std::runtime_error("password change cancelled");
            }
            PasswordChange change;
            change.username = hello.username;
            change.current_password = hello.password;
            change.new_password = new_password;
            pending.stream.send_packet(serialize_packet(change));
            hello.password = new_password;

            welcome_payload = receive_client_control_payload(pending.stream);
            continue;
        }
        break;
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
