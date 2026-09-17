#pragma once

#include "host/session/lobby.hpp"
#include "host/video/media_server.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace archstreamer {

struct HostLaunchPlan {
    GameId game_id;
    GameSessionMode session_mode = GameSessionMode::SinglePlayer;
    std::uint8_t players = 1;
    std::string save_username;
    SeatAssignment seats;
    std::vector<VirtualGamepadIdentity> virtual_identities;
};

struct HostPlayerControllerIdentity {
    std::string name;
    std::string guid;
    std::uint16_t vendor_id = 0;
    std::uint16_t product_id = 0;
};

std::optional<GameId> select_game_for_launch(const GameList& list, const std::string& selector);
void validate_direct_launch(
    GameSessionMode mode,
    std::uint8_t players,
    const GameInfo& game);
SeatAssignment direct_host_seat_assignment(std::uint8_t players);
ClientHello host_player_hello_for_session(
    const std::string& username,
    const GameInfo& game,
    GameSessionMode mode,
    const HostPlayerControllerIdentity& controller);
HostLaunchPlan launch_plan_for_session(const SessionPlan& plan);
HostLaunchPlan launch_plan_for_direct(
    const GameInfo& game,
    GameSessionMode mode,
    std::uint8_t players,
    const std::string& username,
    const std::optional<HostPlayerControllerIdentity>& controller);

std::string media_destination_host(
    const MediaPlanConfig& config,
    const std::string& peer_address);
std::vector<MediaDestination> media_destinations_for_session(
    const MediaPlanConfig& config,
    const SessionPlan& plan);
std::vector<MediaDestination> media_destinations_for_host(
    const MediaPlanConfig& config);

} // namespace archstreamer
