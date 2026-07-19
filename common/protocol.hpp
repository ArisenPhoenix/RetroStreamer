#pragma once

#include "controller_state.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace archstreamer {

constexpr std::uint32_t ProtocolMagic = 0x41525354; // "ARST"
constexpr std::uint16_t ProtocolVersion = 1;
constexpr std::uint8_t MaxRemoteClients = 2;
constexpr std::uint8_t MaxPlayersPerClient = 2;
constexpr std::uint8_t MaxRetroArchPorts = 5; // Ports 0-3 plus a host player if desired.

using ClientId = std::uint8_t;
using LocalPlayerIndex = std::uint8_t;
using RetroArchPort = std::uint8_t;

enum class PacketType : std::uint8_t {
    ClientHello = 1,
    HostWelcome = 2,
    ClientConfig = 3,
    SeatAssignment = 4,
    ControllerInput = 5,
    ViewerHeartbeat = 6,
    Error = 7,
};

enum class ClientRole : std::uint8_t {
    Viewer = 0,
    Player = 1,
};

struct PacketHeader {
    std::uint32_t magic = ProtocolMagic;
    std::uint16_t version = ProtocolVersion;
    PacketType type = PacketType::Error;
    std::uint32_t payload_size = 0;
};

struct ClientHello {
    std::string display_name;
    std::uint8_t requested_players = 0;
    bool wants_video = true;
    bool wants_audio = true;
};

struct HostWelcome {
    ClientId client_id = 0;
    std::uint8_t max_players_for_client = MaxPlayersPerClient;
    bool host_is_player = false;
};

struct ClientConfig {
    std::uint8_t requested_players = 0;
    bool wants_video = true;
    bool wants_audio = true;
};

struct PlayerSeat {
    ClientId client_id = 0;
    LocalPlayerIndex local_player = 0;
    RetroArchPort retroarch_port = 0;
};

struct SeatAssignment {
    std::vector<PlayerSeat> seats;
};

struct ControllerInput {
    ClientId client_id = 0;
    LocalPlayerIndex local_player = 0;
    ControllerState state;
};

struct ViewerHeartbeat {
    ClientId client_id = 0;
    std::uint32_t sequence = 0;
};

struct ErrorPacket {
    std::string message;
};

using PacketPayload = std::variant<
    ClientHello,
    HostWelcome,
    ClientConfig,
    SeatAssignment,
    ControllerInput,
    ViewerHeartbeat,
    ErrorPacket>;

ClientRole role_for_player_count(std::uint8_t requested_players);
bool valid_player_count(std::uint8_t requested_players);

} // namespace archstreamer
