#pragma once

#include "client/session_client.hpp"
#include "common/platform/default_platform.hpp"

#include <filesystem>
#include <functional>
#include <string>
#include <optional>

namespace archstreamer {

PacketPayload receive_client_control_payload(TcpStream& stream);

struct JoinedSession {
    SessionClient session;
    TcpStream stream;
    GameList game_list;
    SessionReady ready;
};

struct PendingSession {
    SessionClient session;
    TcpStream stream;
    GameList game_list;
    std::filesystem::path art_cache_root;
    /** Per-user blocks cache (revision matched independently of catalog offerings). */
    std::uint64_t blocks_revision = 0;
    std::vector<GameId> blocked_game_ids;
};

struct SessionStart {
    SessionStarting starting;
    std::optional<MediaEndpoint> media_endpoint;
};

class ClientSessionService {
public:
    ClientSessionService(std::string host, std::uint16_t control_port);
    ClientSessionService(std::string host, std::uint16_t control_port, std::filesystem::path catalog_cache_path);

    /** Open control TCP, exchange GameList only (no art, no presence). */
    PendingSession begin() const;
    /**
     * Prefetch box/grid/etc. art on short-lived sockets (ArtAssetRequest first).
     * Call only after the GameList handshake socket has moved on (LobbyPresence,
     * ClientHello, or closed) — never while the host is still blocked waiting for
     * the next packet on that connection.
     */
    void sync_catalog_art(const PendingSession& pending) const;
/**
 * After begin()'s unauthenticated GameList: announce LobbyPresence so the host
 * can send CatalogUserBlocks (titles this user cannot play). Does not persist
 * blocks into the shared catalog cache.
 * Do not call this on a socket that will later send ClientHello — presence
 * holds the stream and the lobby discards a subsequent Hello.
 */
    void apply_authenticated_catalog_filter(
        PendingSession& pending,
        std::string_view username,
        std::string_view password) const;
    ActiveSessionInfo active_session_info() const;
    /**
     * Send ClientHello and complete Welcome/Seats/Ready.
     * If the host requires a password change, on_password_change_required(current)
     * must return the new password (non-empty).
     */
    JoinedSession finish_join(
        PendingSession pending,
        ClientHello hello,
        const std::function<std::string(const std::string& current_password)>&
            on_password_change_required = {}) const;
    SessionStart wait_for_starting(TcpStream& stream) const;

private:
    std::string host_;
    std::uint16_t control_port_;
    std::filesystem::path catalog_cache_path_;
};

} // namespace archstreamer
