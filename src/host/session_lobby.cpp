#include "host/session_lobby.hpp"

#include "common/art_transfer.hpp"
#include "common/catalog_paths.hpp"
#include "common/client_logs.hpp"
#include "common/game_identity.hpp"
#include "host/controls_db_sync.hpp"
#include "host/game_meta_store.hpp"
#include "host/save_active_sessions.hpp"
#include "host/save_manager.hpp"
#include "host/user_credentials.hpp"

#include <algorithm>
#include <chrono>
#include <climits>
#include <cstdint>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace archstreamer {

const char* session_mode_name(GameSessionMode mode) {
    switch (mode) {
        case GameSessionMode::SinglePlayer:
            return "singleplayer";
        case GameSessionMode::Multiplayer:
            return "multiplayer";
    }

    return "unknown";
}

PacketPayload receive_control_payload(TcpStream& stream) {
    const auto packet = stream.receive_packet();
    if (!packet.has_value()) {
        throw std::runtime_error("control client disconnected");
    }

    return deserialize_packet(*packet);
}

std::optional<GameInfo> game_info_for(const GameList& list, const GameId& game_id) {
    for (const auto& game : list.games) {
        if (game.id == game_id) {
            return game;
        }
    }

    return std::nullopt;
}

GameList catalog_delta_for_request(const GameList& full_list, const GameListRequest& request) {
    // Prefer the durable catalog_offerings snapshot (hash/revision matched).
    try {
        GameMetaStore store;
        if (store.ready()) {
            const auto revision = store.catalog_offerings_revision();
            if (revision != 0 && request.client_catalog_revision == revision) {
                GameList unchanged;
                unchanged.catalog_revision = revision;
                unchanged.full = false;
                unchanged.deleted_game_ids.clear();
                return unchanged;
            }
            auto offerings = store.load_catalog_offerings();
            if (revision != 0) {
                offerings.full = true;
                offerings.deleted_game_ids.clear();
                return offerings;
            }
        }
    } catch (...) {
    }

    // Fallback when offerings were not rebuilt yet (older host DB / scan failed).
    auto response = full_list;
    response.full = true;
    response.deleted_game_ids.clear();
    for (auto& game : response.games) {
        game.version = catalog_version_display_token(game.version);
    }
    return response;
}

RetroArchPort assigned_player_count(const SeatAssignment& seats) {
    auto players = RetroArchPort{0};
    for (const auto& seat : seats.seats) {
        players = std::max<RetroArchPort>(players, static_cast<RetroArchPort>(seat.retroarch_port + 1));
    }
    return players;
}

std::uint8_t requested_player_count(const SessionPlan& plan) {
    std::uint16_t total = 0;
    if (plan.host_hello.has_value()) {
        total += plan.host_hello->requested_players;
    }
    for (const auto& client : plan.clients) {
        total += client.hello.requested_players;
    }
    return static_cast<std::uint8_t>(std::min<std::uint16_t>(total, UINT8_MAX));
}

ActiveSessionInfo active_session_info_for(
    const SessionPlan& plan,
    bool video_enabled,
    bool audio_enabled) {
    auto connected_players = std::uint8_t{0};
    auto disconnected_players = std::uint8_t{0};
    auto viewer_count = std::uint8_t{0};

    for (const auto& client : plan.clients) {
        if (client.hello.requested_players == 0) {
            if (client.connection_state == SessionConnectionState::Connected) {
                ++viewer_count;
            }
            continue;
        }

        if (client.connection_state == SessionConnectionState::Connected) {
            connected_players = static_cast<std::uint8_t>(connected_players + client.hello.requested_players);
        } else {
            disconnected_players = static_cast<std::uint8_t>(disconnected_players + client.hello.requested_players);
        }
    }

    if (plan.host_hello.has_value() && plan.host_hello->requested_players > 0) {
        connected_players = static_cast<std::uint8_t>(connected_players + plan.host_hello->requested_players);
    }

    return ActiveSessionInfo{
        true,
        plan.selected_game_id,
        plan.session_mode,
        static_cast<std::uint8_t>(assigned_player_count(plan.seats)),
        connected_players,
        disconnected_players,
        viewer_count,
        video_enabled,
        audio_enabled,
        /*active_slots=*/1,
        /*max_slots=*/1,
    };
}

std::uint8_t required_player_count(GameSessionMode mode, const GameInfo& game) {
    if (mode == GameSessionMode::SinglePlayer) {
        return 1;
    }

    return static_cast<std::uint8_t>(std::max<int>(2, game.min_players));
}

bool launch_requirements_satisfied(const SessionPlan& plan, const GameInfo& game) {
    if (plan.session_mode == GameSessionMode::SinglePlayer && !game.supports_singleplayer) {
        throw std::runtime_error("selected game does not support singleplayer");
    }
    if (plan.session_mode == GameSessionMode::Multiplayer && !game.supports_multiplayer) {
        throw std::runtime_error("selected game does not support multiplayer");
    }

    const auto players = requested_player_count(plan);
    if (players > game.max_players) {
        throw std::runtime_error("too many players selected for game");
    }

    return players >= required_player_count(plan.session_mode, game);
}

void send_error_to_session_clients(SessionPlan& plan, std::string_view message) {
    for (auto& client : plan.clients) {
        try {
            client.stream.send_packet(serialize_packet(ErrorPacket{std::string(message)}));
        } catch (const std::exception&) {
        }
    }
}

void send_session_ready_to_clients(SessionPlan& plan) {
    const auto ready = SessionReady{
        plan.selected_game_id,
        plan.session_mode,
        static_cast<std::uint8_t>(assigned_player_count(plan.seats)),
    };

    for (auto& client : plan.clients) {
        client.stream.send_packet(serialize_packet(ready));
    }
}

void send_session_starting_to_clients(SessionPlan& plan) {
    const auto starting = SessionStarting{
        plan.selected_game_id,
        plan.session_mode,
        static_cast<std::uint8_t>(assigned_player_count(plan.seats)),
    };

    for (auto& client : plan.clients) {
        client.stream.send_packet(serialize_packet(starting));
    }
}

void send_media_endpoint_to_client(SessionPlan& plan, ClientId client_id, const MediaEndpoint& endpoint) {
    for (auto& client : plan.clients) {
        if (client.client_id == client_id && (client.hello.wants_video || client.hello.wants_audio)) {
            client.media_endpoint = endpoint;
            client.stream.send_packet(serialize_packet(endpoint));
            return;
        }
    }
}

void send_session_ended_to_clients(SessionPlan& plan, std::string_view reason) {
    const auto ended = SessionEnded{std::string(reason)};

    for (auto& client : plan.clients) {
        try {
            client.stream.send_packet(serialize_packet(ended));
        } catch (const std::exception&) {
        }
    }
}

DiscControlResponse apply_disc_control(SessionPlan& plan, const DiscControlRequest& request) {
    DiscControlResponse response;
    response.disc_count = static_cast<std::uint8_t>(
        std::min<std::size_t>(plan.playlist_discs.size(), 255));
    response.disc_index = plan.current_disc_index;

    if (plan.playlist_discs.size() < 2) {
        response.message = "Active game is not a multi-disc playlist";
        return response;
    }
    if (!request.game_id.empty() && request.game_id != plan.selected_game_id) {
        response.message = "Disc control game_id does not match the active session";
        return response;
    }

    const auto disc_count = static_cast<std::uint8_t>(plan.playlist_discs.size());
    std::uint8_t target = plan.current_disc_index;
    switch (request.action) {
        case DiscControlAction::Next:
            target = static_cast<std::uint8_t>((plan.current_disc_index + 1) % disc_count);
            break;
        case DiscControlAction::Prev:
            target = static_cast<std::uint8_t>(
                (plan.current_disc_index + disc_count - 1) % disc_count);
            break;
        case DiscControlAction::SetIndex:
            if (request.disc_index >= disc_count) {
                response.message = "Disc index out of range";
                return response;
            }
            target = request.disc_index;
            break;
    }

    if (target == plan.current_disc_index) {
        response.ok = true;
        response.message = "Already on requested disc";
        return response;
    }

    const auto port = plan.retroarch_netcmd_port;
    if (!send_retroarch_netcmd("DISK_EJECT_TOGGLE", port)) {
        response.message = "Failed to send DISK_EJECT_TOGGLE to RetroArch";
        return response;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    // Walk forward with DISK_NEXT (wraps) until host-tracked index matches target.
    const auto steps = static_cast<std::uint8_t>(
        (target + disc_count - plan.current_disc_index) % disc_count);
    for (std::uint8_t i = 0; i < steps; ++i) {
        if (!send_retroarch_netcmd("DISK_NEXT", port)) {
            response.message = "Failed to send DISK_NEXT to RetroArch";
            return response;
        }
        plan.current_disc_index = static_cast<std::uint8_t>(
            (plan.current_disc_index + 1) % disc_count);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (!send_retroarch_netcmd("DISK_EJECT_TOGGLE", port)) {
        response.message = "Failed to insert disc (DISK_EJECT_TOGGLE)";
        return response;
    }

    response.ok = true;
    response.disc_index = plan.current_disc_index;
    response.disc_count = disc_count;
    if (plan.current_disc_index < plan.playlist_discs.size()) {
        response.message = "Switched to " + plan.playlist_discs[plan.current_disc_index];
    } else {
        response.message = "Disc switched";
    }
    return response;
}

const SessionClientConnection* session_client_for(const SessionPlan& plan, ClientId client_id) {
    for (const auto& client : plan.clients) {
        if (client.client_id == client_id) {
            return &client;
        }
    }

    return nullptr;
}

std::optional<ControllerInfo> controller_for(const ClientHello& hello, LocalPlayerIndex local_player) {
    for (const auto& controller : hello.controllers) {
        if (controller.local_player == local_player) {
            return controller;
        }
    }

    return std::nullopt;
}

std::string sanitize_virtual_device_text(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (const char character : value) {
        if (character == '"' || character == '\n' || character == '\r' || character == '\t') {
            result.push_back(' ');
        } else {
            result.push_back(character);
        }
    }
    return result;
}

std::string controller_name_for(const ClientHello& hello, LocalPlayerIndex local_player) {
    const auto controller = controller_for(hello, local_player);
    if (controller.has_value() && !controller->name.empty()) {
        return sanitize_virtual_device_text(controller->name);
    }

    return "Controller";
}

std::vector<VirtualGamepadIdentity> virtual_identities_for_session(const SessionPlan& plan) {
    std::vector<VirtualGamepadIdentity> identities(assigned_player_count(plan.seats));

    for (const auto& seat : plan.seats.seats) {
        auto identity = VirtualGamepadIdentity{};
        if (seat.client_id == HostClientId && plan.host_hello.has_value()) {
            identity.name =
                "ArchStreamer " + sanitize_virtual_device_text(plan.host_hello->username) + " " +
                controller_name_for(*plan.host_hello, seat.local_player);
        } else if (const auto* client = session_client_for(plan, seat.client_id); client != nullptr) {
            identity.name =
                "ArchStreamer " + sanitize_virtual_device_text(client->hello.username) + " " +
                controller_name_for(client->hello, seat.local_player);
        }
        identities[seat.retroarch_port] = std::move(identity);
    }

    return identities;
}


void assign_seats_welcome_and_save_username(SessionPlan& plan) {
    SeatManager seat_manager;
    seat_manager.set_host_player_count(plan.host_hello.has_value() ? 1 : 0);
    std::vector<ClientSeatRequest> seat_requests;
    seat_requests.reserve(plan.clients.size());
    for (const auto& client : plan.clients) {
        seat_requests.push_back(ClientSeatRequest{
            client.client_id,
            client.hello.requested_players,
        });
    }
    plan.seats = seat_manager.assign(seat_requests);

    for (auto& client : plan.clients) {
        HostWelcome welcome;
        welcome.client_id = client.client_id;
        welcome.max_players_for_client = MaxPlayersPerClient;
        welcome.host_is_player = plan.host_hello.has_value();
        client.stream.send_packet(serialize_packet(welcome));
        client.stream.send_packet(serialize_packet(plan.seats));
    }
    send_session_ready_to_clients(plan);

    if (plan.host_hello.has_value() && !plan.host_hello->username.empty()) {
        plan.save_username = plan.host_hello->username;
    }
    for (const auto& client : plan.clients) {
        if (client.hello.requested_players > 0) {
            if (plan.save_username.empty()) {
                plan.save_username = client.hello.username;
            }
            break;
        }
    }
    if (plan.save_username.empty() && !plan.clients.empty()) {
        plan.save_username = plan.clients.front().hello.username;
    }
}

void enforce_user_save_stem_for_plan(
    const SessionPlan& plan,
    const std::filesystem::path& save_root) {
    if (save_root.empty() || plan.save_username.empty() || plan.selected_game_id.empty()) {
        return;
    }
    if (user_has_mismatched_save_for_game(
            save_root, plan.save_username, plan.selected_game_id)) {
        throw std::runtime_error(
            "save file name does not match the catalog stem for this game; "
            "rename the save under this user (see Users tab) before playing");
    }
}

void finalize_session_plan_ready(SessionPlan& plan) {
    assign_seats_welcome_and_save_username(plan);
}

SessionPlan make_singleplayer_session_plan(
    ClientId client_id,
    ClientHello hello,
    TcpStream stream,
    const GameList& game_list,
    const std::filesystem::path& save_root) {
    if (!hello.selected_game_id.has_value()) {
        throw std::runtime_error("session client did not select a game");
    }
    const auto selected_game = game_info_for(game_list, *hello.selected_game_id);
    if (!selected_game.has_value()) {
        throw std::runtime_error("session client selected an unknown game");
    }
    // Blocked titles are omitted from the user's catalog; treat a crafted id as unknown.
    try {
        GameMetaStore store;
        if (store.ready() && store.is_user_game_blocked(hello.username, *hello.selected_game_id)) {
            throw std::runtime_error("session client selected an unknown game");
        }
    } catch (const std::runtime_error&) {
        throw;
    } catch (...) {
    }
    if (!save_root.empty()
        && user_has_mismatched_save_for_game(
            save_root, hello.username, *hello.selected_game_id)) {
        throw std::runtime_error(
            "save file name does not match the catalog stem for this game; "
            "rename the save under this user (see Users tab) before playing");
    }
    if (hello.session_mode != GameSessionMode::SinglePlayer) {
        throw std::runtime_error("make_singleplayer_session_plan requires Singleplayer mode");
    }
    if (hello.requested_players == 0) {
        throw std::runtime_error("singleplayer session requires a seated player");
    }

    SessionPlan plan;
    plan.selected_game_id = *hello.selected_game_id;
    plan.session_mode = GameSessionMode::SinglePlayer;
    plan.clients.push_back(SessionClientConnection{
        client_id,
        std::move(hello),
        std::move(stream),
    });
    if (!launch_requirements_satisfied(plan, *selected_game)) {
        throw std::runtime_error("singleplayer launch requirements not satisfied");
    }
    finalize_session_plan_ready(plan);
    return plan;
}

SessionPlan gather_session_clients(
    TcpListener& listener,
    std::uint8_t client_count,
    const GameList& game_list,
    std::chrono::seconds timeout,
    std::optional<ClientHello> host_hello,
    std::function<bool()> should_stop,
    std::filesystem::path art_root,
    std::optional<SessionClientConnection> first_client,
    std::filesystem::path save_root,
    bool allow_new_users) {
    std::cout
        << "Waiting up to " << timeout.count()
        << "s for enough players on shared control listener"
        << " (max clients " << static_cast<int>(client_count) << ").\n";

    SessionPlan plan;
    plan.clients.reserve(client_count);
    plan.host_hello = std::move(host_hello);

    struct LobbyPresenceGuard {
        std::filesystem::path root;
        std::vector<std::uint32_t> client_ids;
        ~LobbyPresenceGuard() {
            // Only clear clients this gather published — never wipe every
            // connected-lobby-* file (that deletes other hosts' catalog Connected).
            for (const auto client_id : client_ids) {
                clear_connected_client(root, client_id, -1);
            }
        }
        void track(std::uint32_t client_id) {
            if (client_id != 0) {
                client_ids.push_back(client_id);
            }
        }
    } lobby_presence{save_root, {}};

    const auto publish_lobby_client = [&](const SessionClientConnection& client) {
        if (save_root.empty() || client.hello.username.empty()) {
            return;
        }
        ConnectedClientPresence presence;
        presence.username = client.hello.username;
        presence.client_id = client.client_id;
        presence.slot_index = -1;
        presence.game_id = client.hello.selected_game_id.value_or(GameId{});
        presence.phase = "lobby";
        presence.seated = client.hello.requested_players > 0;
        publish_connected_client(save_root, presence);
        lobby_presence.track(client.client_id);
    };

    auto selected_game = std::optional<GameId>{};
    auto selected_game_info = std::optional<GameInfo>{};
    auto selected_mode = std::optional<GameSessionMode>{};
    const auto deadline = std::chrono::steady_clock::now() + timeout;

    ClientId client_id = 1;
    if (first_client.has_value()) {
        client_id = static_cast<ClientId>(first_client->client_id + 1);
        selected_game = first_client->hello.selected_game_id;
        selected_mode = first_client->hello.session_mode;
        if (selected_game.has_value()) {
            selected_game_info = game_info_for(game_list, *selected_game);
        }
        plan.clients.push_back(std::move(*first_client));
        publish_lobby_client(plan.clients.back());
        plan.selected_game_id = selected_game.value_or(GameId{});
        plan.session_mode = selected_mode.value_or(GameSessionMode::Multiplayer);
        if (selected_game_info.has_value() &&
            launch_requirements_satisfied(plan, *selected_game_info)) {
            assign_seats_welcome_and_save_username(plan);
            enforce_user_save_stem_for_plan(plan, save_root);
            return plan;
        }
    }

    if (plan.host_hello.has_value()) {
        if (!valid_username(plan.host_hello->username)) {
            throw std::runtime_error("host supplied an invalid username");
        }
        if (plan.host_hello->requested_players != 1) {
            throw std::runtime_error("host player must request exactly one player");
        }
        if (!plan.host_hello->selected_game_id.has_value()) {
            throw std::runtime_error("host player requires a selected game");
        }

        selected_game = plan.host_hello->selected_game_id;
        selected_game_info = game_info_for(game_list, *selected_game);
        if (!selected_game_info.has_value()) {
            throw std::runtime_error("selected game is missing from game list");
        }
        if (!valid_game_player_limits(selected_game_info->min_players, selected_game_info->max_players)) {
            throw std::runtime_error("selected game has invalid player metadata");
        }
        selected_mode = plan.host_hello->session_mode;
        plan.selected_game_id = *selected_game;
        plan.session_mode = *selected_mode;

        if (launch_requirements_satisfied(plan, *selected_game_info)) {
            std::cout
                << "Host player already satisfies "
                << session_mode_name(*selected_mode)
                << " requirements; launching without waiting for remote clients.\n";
            client_count = 0;
        }
    }

    for (; plan.clients.size() < client_count;) {
        if (should_stop && should_stop()) {
            throw std::runtime_error("host stopped");
        }

        // Users-tab Kick of lobby waiters.
        for (std::size_t i = 0; i < plan.clients.size();) {
            auto& client = plan.clients[i];
            if (auto reason = take_connected_client_disconnect_request(
                    save_root, client.client_id, -1);
                reason.has_value()) {
                std::cerr
                    << "Lobby client " << static_cast<int>(client.client_id)
                    << " (" << client.hello.username << ") kicked: " << *reason << '\n';
                try {
                    client.stream = TcpStream{};
                } catch (const std::exception&) {
                }
                clear_connected_client(save_root, client.client_id, -1);
                plan.clients.erase(plan.clients.begin() + static_cast<std::ptrdiff_t>(i));
                continue;
            }
            ++i;
        }

        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            break;
        }

        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        const auto wait_time = std::min(remaining, std::chrono::milliseconds(250));
        auto stream = listener.accept_for(wait_time);
        if (!stream.has_value()) {
            continue;
        }
        std::cout << "Accepted session client " << static_cast<int>(client_id) << ".\n";

        auto accepted_client = false;
        try {
            const auto first_payload = receive_control_payload(*stream);
            if (std::holds_alternative<ActiveSessionInfoRequest>(first_payload)) {
                ActiveSessionInfo idle{};
                idle.active_slots = 0;
                idle.max_slots = 1;
                stream->send_packet(serialize_packet(idle));
                continue;
            }
            if (const auto* art_request = std::get_if<ArtAssetRequest>(&first_payload); art_request != nullptr) {
                stream->send_packet(serialize_packet(load_art_asset_response(
                    art_root,
                    art_request->asset_key,
                    art_request->role,
                    art_request->cached_sha256)));
                continue;
            }
            if (const auto* log_bundle = std::get_if<ClientLogBundle>(&first_payload);
                log_bundle != nullptr) {
                stream->send_packet(serialize_packet(acknowledge_client_log_bundle(*log_bundle)));
                continue;
            }
            if (const auto* password_change = std::get_if<PasswordChange>(&first_payload);
                password_change != nullptr) {
                stream->send_packet(serialize_packet(
                    acknowledge_password_change(save_root, *password_change)));
                continue;
            }
            if (std::holds_alternative<ControlsDbPull>(first_payload)
                || std::holds_alternative<ControlsDbPush>(first_payload)) {
                if (std::holds_alternative<ControlsDbPull>(first_payload)) {
                    ControlsDbResponse response;
                    response.username = std::get<ControlsDbPull>(first_payload).username;
                    response.found = false;
                    stream->send_packet(serialize_packet(response));
                } else {
                    ControlsDbAck ack;
                    ack.username = std::get<ControlsDbPush>(first_payload).username;
                    ack.ok = false;
                    ack.message = "connect with LobbyPresence or join a session first";
                    stream->send_packet(serialize_packet(ack));
                }
                continue;
            }
            const auto* game_list_request = std::get_if<GameListRequest>(&first_payload);
            if (game_list_request == nullptr) {
                throw std::runtime_error("expected GameListRequest from session client");
            }
            stream->send_packet(serialize_packet(catalog_delta_for_request(game_list, *game_list_request)));

            auto next_payload = std::optional<PacketPayload>{};
            while (true) {
                const auto packet = stream->receive_packet();
                if (!packet.has_value()) {
                    next_payload.reset();
                    break;
                }
                auto payload = deserialize_packet(*packet);
                if (const auto* art_request = std::get_if<ArtAssetRequest>(&payload); art_request != nullptr) {
                    stream->send_packet(serialize_packet(load_art_asset_response(
                        art_root,
                        art_request->asset_key,
                        art_request->role,
                        art_request->cached_sha256)));
                    continue;
                }
                next_payload = std::move(payload);
                break;
            }
            if (!next_payload.has_value()) {
                continue;
            }

            const auto* hello = std::get_if<ClientHello>(&*next_payload);
            if (hello == nullptr) {
                throw std::runtime_error("expected ClientHello from session client");
            }
            auto authenticated_hello = *hello;
            if (!valid_username(authenticated_hello.username)) {
                throw std::runtime_error("session client supplied an invalid username");
            }
            authenticate_client_hello(*stream, save_root, authenticated_hello, allow_new_users);
            {
                CatalogUserBlocks blocks;
                try {
                    GameMetaStore store;
                    if (store.ready()) {
                        blocks = store.catalog_user_blocks_for(
                            authenticated_hello.username,
                            authenticated_hello.client_blocks_revision);
                    }
                } catch (...) {
                }
                stream->send_packet(serialize_packet(blocks));
            }
            if (!valid_player_count(authenticated_hello.requested_players)) {
                throw std::runtime_error("session client requested too many players");
            }
            if (authenticated_hello.controllers.size() > authenticated_hello.requested_players) {
                throw std::runtime_error("session client supplied controller metadata for unrequested players");
            }
            if (!authenticated_hello.selected_game_id.has_value()) {
                throw std::runtime_error("session client did not select a game");
            }

            if (!selected_game.has_value()) {
                selected_game = authenticated_hello.selected_game_id;
                selected_game_info = game_info_for(game_list, *selected_game);
                if (!selected_game_info.has_value()) {
                    throw std::runtime_error("session client selected an unknown game");
                }
                if (!valid_game_player_limits(selected_game_info->min_players, selected_game_info->max_players)) {
                    throw std::runtime_error("selected game has invalid player metadata");
                }
            } else if (*selected_game != *authenticated_hello.selected_game_id) {
                throw std::runtime_error("session clients selected different games");
            }

            if (!selected_mode.has_value()) {
                selected_mode = authenticated_hello.session_mode;
            } else if (*selected_mode != authenticated_hello.session_mode) {
                throw std::runtime_error("session clients selected different session modes");
            }

            std::cout
                << "Client " << static_cast<int>(client_id)
                << " username=" << authenticated_hello.username
                << " display=\"" << authenticated_hello.display_name << "\""
                << " mode=" << session_mode_name(authenticated_hello.session_mode)
                << " players=" << static_cast<int>(authenticated_hello.requested_players)
                << " game=\"" << *authenticated_hello.selected_game_id << "\"\n";

            plan.clients.push_back(SessionClientConnection{
                client_id,
                std::move(authenticated_hello),
                std::move(*stream),
            });
            publish_lobby_client(plan.clients.back());
            accepted_client = true;
            ++client_id;

            plan.selected_game_id = *selected_game;
            plan.session_mode = *selected_mode;
            if (launch_requirements_satisfied(plan, *selected_game_info)) {
                break;
            }
        } catch (const std::exception& error) {
            if (accepted_client) {
                send_error_to_session_clients(plan, error.what());
                throw;
            }
            std::cerr << "Rejected session client candidate: " << error.what() << '\n';
        }
    }

    if (!selected_game.has_value() || !selected_game_info.has_value()) {
        throw std::runtime_error("no session client selected a game before timeout");
    }
    if (!launch_requirements_satisfied(plan, *selected_game_info)) {
        std::ostringstream message;
        message
            << "timed out waiting for enough players for " << session_mode_name(plan.session_mode)
            << " session: need " << static_cast<int>(required_player_count(plan.session_mode, *selected_game_info))
            << ", have " << static_cast<int>(requested_player_count(plan));
        send_error_to_session_clients(plan, message.str());
        throw std::runtime_error(message.str());
    }

    assign_seats_welcome_and_save_username(plan);
    enforce_user_save_stem_for_plan(plan, save_root);
    return plan;
}

} // namespace archstreamer
