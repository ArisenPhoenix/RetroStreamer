#pragma once

#include "common/media.hpp"
#include "host/session_lobby.hpp"

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

struct HostMediaPlanConfig {
    bool video = false;
    bool audio = false;
    std::string destination_host = "127.0.0.1";
    bool destination_host_explicit = false;
    std::uint16_t video_port = 5004;
    std::uint16_t audio_port = 6004;
};

struct HostMediaDestination {
    ClientId client_id = 0;
    std::string host;
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
    const HostMediaPlanConfig& config,
    const std::string& peer_address);
std::vector<HostMediaDestination> media_destinations_for_session(
    const HostMediaPlanConfig& config,
    const SessionPlan& plan);
std::vector<HostMediaDestination> media_destinations_for_host(
    const HostMediaPlanConfig& config);
MediaStreamRequest video_request_for_destination(
    const HostMediaPlanConfig& config,
    const HostMediaDestination& destination,
    std::size_t media_index);
MediaStreamRequest audio_request_for_destination(
    const HostMediaPlanConfig& config,
    const HostMediaDestination& destination,
    std::size_t media_index);
std::vector<MediaStreamRequest> video_requests_from_media_destinations(
    const HostMediaPlanConfig& config,
    const std::vector<HostMediaDestination>& destinations);
std::vector<MediaStreamRequest> audio_requests_from_media_destinations(
    const HostMediaPlanConfig& config,
    const std::vector<HostMediaDestination>& destinations);
std::vector<MediaClientStream> media_streams_for_dry_run(
    const HostMediaPlanConfig& config,
    const std::vector<HostMediaDestination>& destinations);

} // namespace archstreamer
