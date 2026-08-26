#include "host/lobby/host_session_helpers.hpp"

#include "common/client_logs.hpp"
#include "common/art_transfer.hpp"
#include "common/catalog_paths.hpp"
#include "common/serialization.hpp"
#include "host/user/controls_db_sync.hpp"
#include "host/db/game_meta_store.hpp"
#include "host/host_app_config.hpp"
#include "host/host_launch_planner.hpp"
#include "host/pair_form_relay.hpp"
#include "host/session/lobby.hpp"
#include "host/user/user_credentials.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <functional>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <variant>
#include <vector>

namespace archstreamer {
namespace {

void send_user_catalog_blocks(
    TcpStream& stream,
    std::string_view username,
    std::uint64_t client_blocks_revision) {
    CatalogUserBlocks blocks;
    try {
        GameMetaStore store;
        if (store.ready()) {
            blocks = store.catalog_user_blocks_for(username, client_blocks_revision);
        }
    } catch (...) {
    }
    stream.send_packet(serialize_packet(blocks));
}

} // namespace

ClientId next_session_client_id(const SessionPlan& plan) {
    auto next_id = ClientId{1};
    for (const auto& client : plan.clients) {
        next_id = std::max<ClientId>(next_id, static_cast<ClientId>(client.info.client_id + 1));
    }
    return next_id;
}

SessionClientConnection* disconnected_player_for_reconnect(SessionPlan& plan, const ClientHello& hello) {
    SessionClientConnection* live = nullptr;
    for (auto& client : plan.clients) {
        if (client.hello.requested_players == 0) {
            continue;
        }
        if (client.info.username != hello.username) {
            continue;
        }
        if (client.hello.requested_players != hello.requested_players) {
            continue;
        }
        if (client.lifecycle.connection_state == SessionConnectionState::Disconnected) {
            return &client;
        }
        if (client.lifecycle.connection_state == SessionConnectionState::Connected) {
            live = &client;
        }
    }
    return live;
}

LiveSessionJoinTarget resolve_live_session_join_target(
    SessionPlan& plan,
    const ClientHello& hello,
    bool reconnect_requested) {
    LiveSessionJoinTarget target;
    target.client_id = next_session_client_id(plan);
    target.udp_session_token = make_udp_session_token();
    if (reconnect_requested || hello.requested_players > 0) {
        target.reconnecting_client = disconnected_player_for_reconnect(plan, hello);
        if (target.reconnecting_client == nullptr && hello.requested_players > 0) {
            throw std::runtime_error("active sessions only accept late viewers or reconnecting players");
        }
        if (target.reconnecting_client != nullptr) {
            target.client_id = target.reconnecting_client->info.client_id;
            target.udp_session_token = target.reconnecting_client->info.udp_session_token;
        }
    }
    return target;
}

MediaEndpoint send_live_session_join_handshake(const LiveSessionJoinHandshake& join) {
    auto welcome = HostWelcome{};
    welcome.client_id = join.client_id;
    welcome.max_players_for_client = MaxPlayersPerClient;
    welcome.host_is_player = join.plan.host_hello.has_value();
    welcome.udp_session_token = join.udp_session_token;
    join.stream.send_packet(serialize_packet(welcome));
    join.stream.send_packet(serialize_packet(join.plan.seats));
    join.stream.send_packet(serialize_packet(SessionReady{
        join.plan.game.selected_game_id,
        join.plan.game.session_mode,
        static_cast<std::uint8_t>(assigned_player_count(join.plan.seats)),
    }));

    const auto destination_host = media_destination_host(
        join.media_config,
        join.stream.peer_address());
    auto endpoint = MediaEndpoint{};
    if (join.hello.wants_video || join.hello.wants_audio) {
        endpoint = join.media_server.add_client(
            join.client_id,
            destination_host,
            join.media_index,
            join.hello.wants_video,
            join.hello.wants_audio);
        if (!endpoint.video_uri.empty() || !endpoint.audio_uri.empty()) {
            ++join.media_index;
            join.stream.send_packet(serialize_packet(endpoint));
        }
    }

    join.stream.send_packet(serialize_packet(SessionStarting{
        join.plan.game.selected_game_id,
        join.plan.game.session_mode,
        static_cast<std::uint8_t>(assigned_player_count(join.plan.seats)),
    }));
    return endpoint;
}

void reset_reconnected_session_client(
    SessionClientConnection& client,
    const ClientHello& hello,
    TcpStream&& stream,
    const SessionPlan& plan,
    const MediaEndpoint& endpoint) {
    client.hello = hello;
    const auto udp_session_token = client.info.udp_session_token;
    client.info = client_info_for(client.info.client_id, hello);
    client.info.udp_session_token = udp_session_token;
    client.lifecycle.stream = std::move(stream);
    client.lifecycle.connection_state = SessionConnectionState::Connected;
    client.lifecycle.last_seen = std::chrono::steady_clock::now();
    client.lifecycle.disconnected_at = {};
    client.lifecycle.disconnect_reason.clear();
    client.stream_preferences.applied_tier = plan.stream.video_tier;
    client.stream_preferences.applied_size = plan.stream.video_size;
    client.stream_preferences.applied_feel = plan.stream.video_feel;
    client.stream_preferences.applied_bitrate = plan.stream.video_bitrate;
    client.stream_preferences.adaptive_fps_cap = MediaStreamFps::Auto;
    client.stream_preferences.applied_fps = plan.stream.video_fps;
    client.video_cutover.pending_tier.reset();
    client.video_cutover.pending_size.reset();
    client.video_cutover.pending_feel.reset();
    client.video_cutover.pending_bitrate.reset();
    client.video_cutover.pending_fps.reset();
    client.video_cutover.pending_video_uri.reset();
    client.video_cutover.started = {};
    client.video_cutover.failures = 0;
    client.video_cutover.suppressed = false;
    client.video_health.positive_video_heartbeats = 0;
    client.video_health.initial_video_settings_ready = false;
    client.video_health.initial_video_settings_defer_logged = false;
    // add_client restarts the shared encode; arm stall recovery window.
    client.video_health.last_video_reconfigure = std::chrono::steady_clock::now();
    client.video_health.video_zero_frame_streak = 0;
    if (!endpoint.video_uri.empty() || !endpoint.audio_uri.empty()) {
        client.media.endpoint = endpoint;
    } else {
        client.media.endpoint.reset();
    }
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
        if (const auto* log_bundle = std::get_if<ClientLogBundle>(&first_payload);
            log_bundle != nullptr) {
            stream->send_packet(serialize_packet(acknowledge_client_log_bundle(*log_bundle)));
            return;
        }
        if (const auto* password_change = std::get_if<PasswordChange>(&first_payload);
            password_change != nullptr) {
            stream->send_packet(serialize_packet(
                acknowledge_password_change(config.save_root, *password_change)));
            return;
        }
        if (std::holds_alternative<ControlsDbPull>(first_payload)
            || std::holds_alternative<ControlsDbPush>(first_payload)) {
            // Catalog one-shot sockets are unauthenticated; require LobbyPresence or a session.
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
            return;
        }
        if (is_pair_form_relay_packet(first_payload)) {
            auto reply = handle_pair_form_relay_packet(first_payload);
            if (!reply.empty()) {
                stream->send_packet(reply);
            }
            return;
        }
        const auto art_root = config.art_root.empty()
            ? (config.rom_root.parent_path() / "Art")
            : config.art_root;
        if (const auto* art_request = std::get_if<ArtAssetRequest>(&first_payload); art_request != nullptr) {
            stream->send_packet(serialize_packet(load_art_asset_response(
                art_root,
                art_request->asset_key,
                art_request->role,
                art_request->cached_sha256)));
            return;
        }
        const auto* game_list_request = std::get_if<GameListRequest>(&first_payload);
        if (game_list_request == nullptr) {
            const auto got = static_cast<int>(std::visit(
                [](const auto& payload) { return packet_type_for(payload); },
                first_payload));
            throw std::runtime_error(
                "expected GameListRequest from active-session client (got packet type " +
                std::to_string(got) + ")");
        }
        stream->send_packet(serialize_packet(catalog_delta_for_request(game_list, *game_list_request)));

        auto next_payload = std::optional<PacketPayload>{};
        while (true) {
            const auto packet = stream->receive_packet();
            if (!packet.has_value()) {
                return;
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
            return;
        }

        const auto* hello = std::get_if<ClientHello>(&*next_payload);
        if (hello == nullptr) {
            throw std::runtime_error("expected ClientHello from active-session client");
        }
        auto authenticated_hello = *hello;
        if (!valid_username(authenticated_hello.username)) {
            throw std::runtime_error("active-session client supplied an invalid username");
        }
        authenticate_client_hello(*stream, config.save_root, authenticated_hello, config.allow_new_users);
        send_user_catalog_blocks(
            *stream,
            authenticated_hello.username,
            authenticated_hello.client_blocks_revision);
        if (!authenticated_hello.selected_game_id.has_value() ||
            *authenticated_hello.selected_game_id != plan.game.selected_game_id) {
            throw std::runtime_error("active-session client selected a different game");
        }
        if (authenticated_hello.session_mode != plan.game.session_mode) {
            throw std::runtime_error("active-session client selected a different session mode");
        }
        if (!valid_player_count(authenticated_hello.requested_players)) {
            throw std::runtime_error("active-session client requested too many players");
        }
        if (authenticated_hello.controllers.size() > authenticated_hello.requested_players) {
            throw std::runtime_error("active-session client supplied controller metadata for unrequested players");
        }

        const auto join = resolve_live_session_join_target(plan, authenticated_hello);
        auto endpoint = send_live_session_join_handshake(LiveSessionJoinHandshake{
            *stream,
            authenticated_hello,
            join.client_id,
            join.udp_session_token,
            plan,
            media_plan_config_for(config),
            media_index,
            media_server,
        });

        if (join.reconnecting_client != nullptr) {
            reset_reconnected_session_client(
                *join.reconnecting_client,
                authenticated_hello,
                std::move(*stream),
                plan,
                endpoint);
            std::cout
                << "Player " << static_cast<int>(join.client_id)
                << " reconnected username=" << authenticated_hello.username << ".\n";
        } else {
            plan.clients.push_back(make_session_client(
                join.client_id,
                authenticated_hello,
                std::move(*stream)));
            plan.clients.back().info.udp_session_token = join.udp_session_token;
            std::cout
                << "Late viewer " << static_cast<int>(join.client_id)
                << " joined username=" << authenticated_hello.username << ".\n";
        }
    } catch (const std::exception& error) {
        try {
            stream->send_packet(serialize_packet(ErrorPacket{error.what()}));
        } catch (const std::exception&) {
        }
        std::cerr << "Rejected active-session join: " << error.what() << '\n';
    }
}

std::optional<AcceptedControlHello> try_accept_control_hello(
    TcpListener& listener,
    const GameList& game_list,
    const std::filesystem::path& art_root,
    const std::function<ActiveSessionInfo()>& active_info_fn,
    const std::filesystem::path& save_root,
    bool allow_new_users) {
    auto stream = listener.accept_for(std::chrono::milliseconds(0));
    if (!stream.has_value()) {
        return std::nullopt;
    }

    std::cout << "Accepted control connection.\n";
    try {
        const auto first_payload = receive_control_payload(*stream);
        if (std::holds_alternative<ActiveSessionInfoRequest>(first_payload)) {
            ActiveSessionInfo info{};
            if (active_info_fn) {
                info = active_info_fn();
            }
            stream->send_packet(serialize_packet(info));
            return AcceptedControlHello{};
        }
        const auto& resolved_art = art_root;
        if (const auto* art_request = std::get_if<ArtAssetRequest>(&first_payload);
            art_request != nullptr) {
            stream->send_packet(serialize_packet(load_art_asset_response(
                resolved_art,
                art_request->asset_key,
                art_request->role,
                art_request->cached_sha256)));
            return AcceptedControlHello{};
        }
        if (const auto* log_bundle = std::get_if<ClientLogBundle>(&first_payload);
            log_bundle != nullptr) {
            stream->send_packet(serialize_packet(acknowledge_client_log_bundle(*log_bundle)));
            return AcceptedControlHello{};
        }
        if (const auto* password_change = std::get_if<PasswordChange>(&first_payload);
            password_change != nullptr) {
            stream->send_packet(serialize_packet(
                acknowledge_password_change(save_root, *password_change)));
            return AcceptedControlHello{};
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
            return AcceptedControlHello{};
        }
        const auto* game_list_request = std::get_if<GameListRequest>(&first_payload);
        if (game_list_request == nullptr) {
            const auto got = static_cast<int>(std::visit(
                [](const auto& payload) { return packet_type_for(payload); },
                first_payload));
            throw std::runtime_error(
                "expected GameListRequest from control client (got packet type " +
                std::to_string(got) + ")");
        }
        stream->send_packet(serialize_packet(catalog_delta_for_request(game_list, *game_list_request)));

        auto next_payload = std::optional<PacketPayload>{};
        while (true) {
            const auto packet = stream->receive_packet();
            if (!packet.has_value()) {
                return AcceptedControlHello{};
            }
            auto payload = deserialize_packet(*packet);
            if (const auto* art_request = std::get_if<ArtAssetRequest>(&payload);
                art_request != nullptr) {
                stream->send_packet(serialize_packet(load_art_asset_response(
                    resolved_art,
                    art_request->asset_key,
                    art_request->role,
                    art_request->cached_sha256)));
                continue;
            }
            next_payload = std::move(payload);
            break;
        }
        if (!next_payload.has_value()) {
            return AcceptedControlHello{};
        }

        if (const auto* presence = std::get_if<LobbyPresence>(&*next_payload); presence != nullptr) {
            if (!valid_username(presence->username)) {
                throw std::runtime_error("lobby presence supplied an invalid username");
            }
            // Auth without creating a save profile / session — MustChange is still Connected.
            const auto auth = verify_or_create_on_hello(
                save_root,
                presence->username,
                presence->password,
                allow_new_users);
            if (auth != UserAuthResult::Ok && auth != UserAuthResult::MustChange) {
                throw std::runtime_error("lobby presence authentication failed");
            }
            // Per-user blocks (hash-matched); shared catalog offerings stay complete.
            send_user_catalog_blocks(
                *stream,
                presence->username,
                presence->client_blocks_revision);
            AcceptedControlHello accepted;
            auto info = ClientInfo{};
            info.client_id = UnassignedClientId;
            info.username = presence->username;
            accepted.presence = ControlClientConnection{
                std::move(info),
                std::move(*stream),
            };
            return accepted;
        }

        const auto* hello = std::get_if<ClientHello>(&*next_payload);
        if (hello == nullptr) {
            throw std::runtime_error("expected ClientHello or LobbyPresence from control client");
        }
        auto authenticated_hello = *hello;
        if (!valid_username(authenticated_hello.username)) {
            throw std::runtime_error("control client supplied an invalid username");
        }
        authenticate_client_hello(*stream, save_root, authenticated_hello, allow_new_users);
        if (!valid_player_count(authenticated_hello.requested_players)) {
            throw std::runtime_error("control client requested too many players");
        }
        if (authenticated_hello.controllers.size() > authenticated_hello.requested_players) {
            throw std::runtime_error("control client supplied controller metadata for unrequested players");
        }
        if (!authenticated_hello.selected_game_id.has_value()) {
            throw std::runtime_error("control client did not select a game");
        }
        send_user_catalog_blocks(
            *stream,
            authenticated_hello.username,
            authenticated_hello.client_blocks_revision);

        AcceptedControlHello accepted;
        accepted.client = AuthenticatedSessionRequest{
            std::move(authenticated_hello),
            std::move(*stream),
        };
        return accepted;
    } catch (const std::exception& error) {
        try {
            stream->send_packet(serialize_packet(ErrorPacket{error.what()}));
        } catch (const std::exception&) {
        }
        std::cerr << "Rejected control client: " << error.what() << '\n';
        return AcceptedControlHello{};
    }
}

} // namespace archstreamer
