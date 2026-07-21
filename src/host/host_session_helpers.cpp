#include "host/host_session_helpers.hpp"

#include "common/serialization.hpp"
#include "host/host_app_config.hpp"
#include "host/host_launch_planner.hpp"
#include "host/session_service.hpp"

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <variant>
#include <vector>

namespace archstreamer {

ClientId next_session_client_id(const SessionPlan& plan) {
    auto next_id = ClientId{1};
    for (const auto& client : plan.clients) {
        next_id = std::max<ClientId>(next_id, static_cast<ClientId>(client.client_id + 1));
    }
    return next_id;
}

SessionClientConnection* disconnected_player_for_reconnect(SessionPlan& plan, const ClientHello& hello) {
    for (auto& client : plan.clients) {
        if (client.connection_state != SessionConnectionState::Disconnected) {
            continue;
        }
        if (client.hello.requested_players == 0) {
            continue;
        }
        if (client.hello.username != hello.username) {
            continue;
        }
        if (client.hello.requested_players != hello.requested_players) {
            continue;
        }
        return &client;
    }

    return nullptr;
}

std::string hex_vid_pid(std::uint16_t vendor_id, std::uint16_t product_id) {
    std::ostringstream out;
    out
        << "0x" << std::hex << std::setw(4) << std::setfill('0') << vendor_id
        << "/0x" << std::hex << std::setw(4) << std::setfill('0') << product_id;
    return out.str();
}

HostPlayerControllerIdentity host_player_controller_identity(const ControllerDevice& device) {
    return HostPlayerControllerIdentity{
        device.name,
        device.guid,
        device.vendor_id,
        device.product_id,
    };
}

std::optional<std::string> sdl_ignore_list_for_session(const SessionPlan& plan) {
    std::vector<std::string> ignored;
    std::string result;
    const auto add_controller = [&](const ControllerInfo& controller) {
        if (controller.vendor_id == 0 || controller.product_id == 0) {
            return;
        }
        const auto vid_pid = hex_vid_pid(controller.vendor_id, controller.product_id);
        if (std::find(ignored.begin(), ignored.end(), vid_pid) != ignored.end()) {
            return;
        }
        ignored.push_back(vid_pid);
        if (!result.empty()) {
            result += ",";
        }
        result += vid_pid;
    };

    if (plan.host_hello.has_value()) {
        for (const auto& controller : plan.host_hello->controllers) {
            add_controller(controller);
        }
    }
    for (const auto& client : plan.clients) {
        for (const auto& controller : client.hello.controllers) {
            add_controller(controller);
        }
    }

    if (result.empty()) {
        return std::nullopt;
    }

    return result;
}

void poll_active_session_joins(
    TcpListener& listener,
    SessionPlan& plan,
    const GameList& game_list,
    const HostAppConfig& config,
    std::size_t& media_index,
    MediaServer& media_server) {
    auto stream = listener.accept_for(std::chrono::milliseconds(0));
    if (!stream.has_value()) {
        return;
    }

    std::cout << "Accepted active-session join candidate.\n";

    try {
        const auto first_payload = receive_control_payload(*stream);
        if (std::holds_alternative<ActiveSessionInfoRequest>(first_payload)) {
            stream->send_packet(serialize_packet(active_session_info_for(
                plan,
                config.video,
                config.audio)));
            return;
        }
        const auto* game_list_request = std::get_if<GameListRequest>(&first_payload);
        if (game_list_request == nullptr) {
            throw std::runtime_error("expected GameListRequest from active-session client");
        }
        stream->send_packet(serialize_packet(catalog_delta_for_request(game_list, *game_list_request)));

        const auto second_payload = receive_control_payload(*stream);
        const auto* hello = std::get_if<ClientHello>(&second_payload);
        if (hello == nullptr) {
            throw std::runtime_error("expected ClientHello from active-session client");
        }
        if (!valid_username(hello->username)) {
            throw std::runtime_error("active-session client supplied an invalid username");
        }
        if (!hello->selected_game_id.has_value() || *hello->selected_game_id != plan.selected_game_id) {
            throw std::runtime_error("active-session client selected a different game");
        }
        if (hello->session_mode != plan.session_mode) {
            throw std::runtime_error("active-session client selected a different session mode");
        }
        if (!valid_player_count(hello->requested_players)) {
            throw std::runtime_error("active-session client requested too many players");
        }
        if (hello->controllers.size() > hello->requested_players) {
            throw std::runtime_error("active-session client supplied controller metadata for unrequested players");
        }

        auto* reconnected_player = static_cast<SessionClientConnection*>(nullptr);
        auto client_id = next_session_client_id(plan);
        if (hello->requested_players > 0) {
            reconnected_player = disconnected_player_for_reconnect(plan, *hello);
            if (reconnected_player == nullptr) {
                throw std::runtime_error("active sessions only accept late viewers or reconnecting players");
            }
            client_id = reconnected_player->client_id;
        }

        auto welcome = HostWelcome{};
        welcome.client_id = client_id;
        welcome.max_players_for_client = MaxPlayersPerClient;
        welcome.host_is_player = plan.host_hello.has_value();
        stream->send_packet(serialize_packet(welcome));
        stream->send_packet(serialize_packet(plan.seats));
        stream->send_packet(serialize_packet(SessionReady{
            plan.selected_game_id,
            plan.session_mode,
            static_cast<std::uint8_t>(assigned_player_count(plan.seats)),
        }));

        const auto destination_host = media_destination_host(
            media_plan_config_for(config),
            stream->peer_address());
        auto endpoint = MediaEndpoint{};
        if (hello->wants_video || hello->wants_audio) {
            endpoint = media_server.add_client(
                client_id,
                destination_host,
                media_index,
                hello->wants_video,
                hello->wants_audio);
            if (!endpoint.video_uri.empty() || !endpoint.audio_uri.empty()) {
                ++media_index;
                stream->send_packet(serialize_packet(endpoint));
            }
        }

        stream->send_packet(serialize_packet(SessionStarting{
            plan.selected_game_id,
            plan.session_mode,
            static_cast<std::uint8_t>(assigned_player_count(plan.seats)),
        }));

        if (reconnected_player != nullptr) {
            reconnected_player->hello = *hello;
            reconnected_player->stream = std::move(*stream);
            reconnected_player->connection_state = SessionConnectionState::Connected;
            reconnected_player->last_seen = std::chrono::steady_clock::now();
            reconnected_player->disconnected_at = {};
            std::cout
                << "Player " << static_cast<int>(client_id)
                << " reconnected username=" << hello->username << ".\n";
        } else {
            plan.clients.push_back(SessionClientConnection{
                client_id,
                *hello,
                std::move(*stream),
            });
            std::cout
                << "Late viewer " << static_cast<int>(client_id)
                << " joined username=" << hello->username << ".\n";
        }
    } catch (const std::exception& error) {
        try {
            stream->send_packet(serialize_packet(ErrorPacket{error.what()}));
        } catch (const std::exception&) {
        }
        std::cerr << "Rejected active-session join: " << error.what() << '\n';
    }
}

} // namespace archstreamer
