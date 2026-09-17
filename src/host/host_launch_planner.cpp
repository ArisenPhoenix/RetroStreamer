#include "host/host_launch_planner.hpp"

#include <algorithm>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace archstreamer {
namespace {

bool is_decimal_number(std::string_view value) {
    if (value.empty()) {
        return false;
    }

    for (const auto character : value) {
        if (character < '0' || character > '9') {
            return false;
        }
    }

    return true;
}

} // namespace

std::optional<GameId> select_game_for_launch(const GameList& list, const std::string& selector) {
    if (is_decimal_number(selector)) {
        const auto index = static_cast<std::size_t>(std::stoul(selector));
        if (index >= list.games.size()) {
            return std::nullopt;
        }

        return list.games[index].id;
    }

    for (const auto& game : list.games) {
        if (game.id == selector) {
            return game.id;
        }
    }

    return std::nullopt;
}

void validate_direct_launch(
    GameSessionMode mode,
    std::uint8_t players,
    const GameInfo& game) {
    if (!valid_game_player_limits(game.min_players, game.max_players)) {
        throw std::runtime_error("selected game has invalid player metadata");
    }
    if (mode == GameSessionMode::SinglePlayer && !game.supports_singleplayer) {
        throw std::runtime_error("selected game does not support singleplayer");
    }
    if (mode == GameSessionMode::Multiplayer && !game.supports_multiplayer) {
        throw std::runtime_error("selected game does not support multiplayer");
    }
    if (players > game.max_players) {
        throw std::runtime_error("too many players selected for game");
    }

    const auto required = required_player_count(mode, game);
    if (players < required) {
        std::ostringstream message;
        message
            << "not enough players selected for " << session_mode_name(mode)
            << " session: need " << static_cast<int>(required)
            << ", got " << static_cast<int>(players);
        throw std::runtime_error(message.str());
    }
}

SeatAssignment direct_host_seat_assignment(std::uint8_t players) {
    SeatAssignment assignment;
    for (LocalPlayerIndex player = 0; player < players; ++player) {
        assignment.seats.push_back(PlayerSeat{HostClientId, player, player});
    }

    return assignment;
}

ClientHello host_player_hello_for_session(
    const std::string& username,
    const GameInfo& game,
    GameSessionMode mode,
    const HostPlayerControllerIdentity& controller) {
    if (!valid_game_player_limits(game.min_players, game.max_players)) {
        throw std::runtime_error("selected game has invalid player metadata");
    }
    if (mode == GameSessionMode::SinglePlayer && !game.supports_singleplayer) {
        throw std::runtime_error("selected game does not support singleplayer");
    }
    if (mode == GameSessionMode::Multiplayer && !game.supports_multiplayer) {
        throw std::runtime_error("selected game does not support multiplayer");
    }
    if (game.max_players < 1) {
        throw std::runtime_error("selected game does not have room for the host player");
    }
    if (!valid_username(username)) {
        throw std::runtime_error("username must be 1-64 characters and contain only letters, numbers, underscores, or hyphens");
    }

    return ClientHello{
        username,
        username,
        game.id,
        mode,
        1,
        std::vector<ControllerInfo>{
            ControllerInfo{
                0,
                controller.name,
                controller.guid,
                controller.vendor_id,
                controller.product_id,
            },
        },
        true,
        true,
    };
}

HostLaunchPlan launch_plan_for_session(const SessionPlan& plan) {
    auto launch_plan = HostLaunchPlan{};
    launch_plan.game_id = plan.game.selected_game_id;
    launch_plan.session_mode = plan.game.session_mode;
    launch_plan.players = static_cast<std::uint8_t>(assigned_player_count(plan.seats));
    launch_plan.save_username = plan.game.save_username;
    launch_plan.seats = plan.seats;
    launch_plan.virtual_identities = virtual_identities_for_session(plan);
    return launch_plan;
}

HostLaunchPlan launch_plan_for_direct(
    const GameInfo& game,
    GameSessionMode mode,
    std::uint8_t players,
    const std::string& username,
    const std::optional<HostPlayerControllerIdentity>& controller) {
    validate_direct_launch(mode, players, game);
    if (!valid_username(username)) {
        throw std::runtime_error("username must be 1-64 characters and contain only letters, numbers, underscores, or hyphens");
    }

    auto virtual_identity = VirtualGamepadIdentity{};
    if (controller.has_value()) {
        virtual_identity.name = "ArchStreamer " + sanitize_virtual_device_text(controller->name);
    }

    auto launch_plan = HostLaunchPlan{};
    launch_plan.game_id = game.id;
    launch_plan.session_mode = mode;
    launch_plan.players = players;
    launch_plan.save_username = username;
    launch_plan.seats = direct_host_seat_assignment(players);
    launch_plan.virtual_identities.assign(players, virtual_identity);
    return launch_plan;
}

std::string media_destination_host(
    const MediaPlanConfig& config,
    const std::string& peer_address) {
    if (config.destination_host_explicit) {
        return config.destination_host;
    }
    if (!peer_address.empty()) {
        return peer_address;
    }

    return config.destination_host;
}

std::vector<MediaDestination> media_destinations_for_session(
    const MediaPlanConfig& config,
    const SessionPlan& plan) {
    auto destinations = std::vector<MediaDestination>{};
    if (config.include_host_loopback && (config.video || config.audio)) {
        destinations.push_back(MediaDestination{
            HostClientId,
            "127.0.0.1",
        });
    }
    for (const auto& client : plan.clients) {
        if (client.info.client_id == HostClientId) {
            continue;
        }
        if ((config.video && client.hello.wants_video) || (config.audio && client.hello.wants_audio)) {
            destinations.push_back(MediaDestination{
                client.info.client_id,
                media_destination_host(config, client.lifecycle.stream.peer_address()),
            });
        }
    }
    return destinations;
}

std::vector<MediaDestination> media_destinations_for_host(
    const MediaPlanConfig& config) {
    if (!config.video && !config.audio) {
        return {};
    }
    if (!config.include_host_loopback && !config.destination_host_explicit) {
        return {};
    }
    return std::vector<MediaDestination>{
        MediaDestination{
            HostClientId,
            config.destination_host,
        },
    };
}

} // namespace archstreamer
