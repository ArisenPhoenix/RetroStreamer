#include "host/session_lobby.hpp"

#include "common/art_transfer.hpp"
#include "common/catalog_paths.hpp"

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
    auto response = GameList{};
    response.catalog_revision = full_list.catalog_revision;
    response.full = request.client_catalog_revision == 0;

    for (const auto& game : full_list.games) {
        if (response.full || game.updated_at > request.client_catalog_revision) {
            response.games.push_back(game);
        }
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

SessionPlan wait_for_session_clients(
    std::uint16_t control_port,
    std::uint8_t client_count,
    const GameList& game_list,
    std::chrono::seconds timeout,
    std::optional<ClientHello> host_hello,
    std::function<bool()> should_stop,
    std::filesystem::path art_root) {
    if (art_root.empty()) {
        art_root = DefaultArtRoot;
    }
    TcpListener listener(control_port);
    std::cout
        << "Waiting up to " << timeout.count()
        << "s for enough players on TCP port " << control_port
        << " (max clients " << static_cast<int>(client_count) << ").\n";

    SessionPlan plan;
    plan.clients.reserve(client_count);
    plan.host_hello = std::move(host_hello);

    auto selected_game = std::optional<GameId>{};
    auto selected_game_info = std::optional<GameInfo>{};
    auto selected_mode = std::optional<GameSessionMode>{};
    const auto deadline = std::chrono::steady_clock::now() + timeout;

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
            throw std::runtime_error("host selected an unknown game");
        }
        if (!valid_game_player_limits(selected_game_info->min_players, selected_game_info->max_players)) {
            throw std::runtime_error("selected game has invalid player metadata");
        }
        selected_mode = plan.host_hello->session_mode;
        plan.selected_game_id = *selected_game;
        plan.session_mode = *selected_mode;

        // Host alone can satisfy singleplayer (and some multiplayer setups later).
        // Launch immediately instead of holding the lobby open for optional remotes.
        if (launch_requirements_satisfied(plan, *selected_game_info)) {
            std::cout
                << "Host player already satisfies "
                << session_mode_name(*selected_mode)
                << " requirements; launching without waiting for remote clients.\n";
            client_count = 0;
        }
    }

    for (ClientId client_id = 1; client_id <= client_count;) {
        if (should_stop && should_stop()) {
            throw std::runtime_error("host stopped");
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
                stream->send_packet(serialize_packet(ActiveSessionInfo{}));
                continue;
            }
            if (const auto* art_request = std::get_if<ArtAssetRequest>(&first_payload); art_request != nullptr) {
                stream->send_packet(serialize_packet(load_art_asset_response(
                    art_root,
                    art_request->asset_key,
                    art_request->role)));
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
                    // Catalog/art-only client disconnected.
                    next_payload.reset();
                    break;
                }
                auto payload = deserialize_packet(*packet);
                if (const auto* art_request = std::get_if<ArtAssetRequest>(&payload); art_request != nullptr) {
                    stream->send_packet(serialize_packet(load_art_asset_response(
                        art_root,
                        art_request->asset_key,
                        art_request->role)));
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
            if (!valid_username(hello->username)) {
                throw std::runtime_error("session client supplied an invalid username");
            }
            if (!valid_player_count(hello->requested_players)) {
                throw std::runtime_error("session client requested too many players");
            }
            if (hello->controllers.size() > hello->requested_players) {
                throw std::runtime_error("session client supplied controller metadata for unrequested players");
            }
            if (!hello->selected_game_id.has_value()) {
                throw std::runtime_error("session client did not select a game");
            }

            if (!selected_game.has_value()) {
                selected_game = hello->selected_game_id;
                selected_game_info = game_info_for(game_list, *selected_game);
                if (!selected_game_info.has_value()) {
                    throw std::runtime_error("session client selected an unknown game");
                }
                if (!valid_game_player_limits(selected_game_info->min_players, selected_game_info->max_players)) {
                    throw std::runtime_error("selected game has invalid player metadata");
                }
            } else if (*selected_game != *hello->selected_game_id) {
                throw std::runtime_error("session clients selected different games");
            }

            if (!selected_mode.has_value()) {
                selected_mode = hello->session_mode;
            } else if (*selected_mode != hello->session_mode) {
                throw std::runtime_error("session clients selected different session modes");
            }

            std::cout
                << "Client " << static_cast<int>(client_id)
                << " username=" << hello->username
                << " display=\"" << hello->display_name << "\""
                << " mode=" << session_mode_name(hello->session_mode)
                << " players=" << static_cast<int>(hello->requested_players)
                << " game=\"" << *hello->selected_game_id << "\"\n";

            plan.clients.push_back(SessionClientConnection{
                client_id,
                *hello,
                std::move(*stream),
            });
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

    return plan;
}

} // namespace archstreamer
