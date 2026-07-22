#pragma once

#include "common/controller_state.hpp"
#include "common/media.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace archstreamer {

constexpr std::uint32_t ProtocolMagic = 0x41525354; // "ARST"
constexpr std::uint16_t ProtocolVersion = 8;
constexpr std::uint8_t MaxRemoteClients = 2;
constexpr std::uint8_t MaxPlayersPerClient = 2;
constexpr std::uint8_t MaxRetroArchPorts = 5; // Ports 0-3 plus a host player if desired.

using ClientId = std::uint8_t;
using LocalPlayerIndex = std::uint8_t;
using RetroArchPort = std::uint8_t;

constexpr ClientId HostClientId = 0;

enum class PacketType : std::uint8_t {
    ClientHello = 1,
    HostWelcome = 2,
    ClientConfig = 3,
    SeatAssignment = 4,
    ControllerInput = 5,
    ViewerHeartbeat = 6,
    Error = 7,
    GameListRequest = 8,
    GameList = 9,
    SessionReady = 10,
    SessionStarting = 11,
    SessionEnded = 12,
    MediaEndpoint = 13,
    ActiveSessionInfoRequest = 14,
    ActiveSessionInfo = 15,
    ArtAssetRequest = 16,
    ArtAssetResponse = 17,
};

enum class ClientRole : std::uint8_t {
    Viewer = 0,
    Player = 1,
};

enum class GameSessionMode : std::uint8_t {
    SinglePlayer = 0,
    Multiplayer = 1,
};

struct PacketHeader {
    std::uint32_t magic = ProtocolMagic;
    std::uint16_t version = ProtocolVersion;
    PacketType type = PacketType::Error;
    std::uint32_t payload_size = 0;
};

using GameId = std::string;

struct GameInfo {
    GameId id;
    std::string identity_key;
    std::string asset_key;
    std::string display_name;
    std::string system_name;
    std::string system_key;
    std::string core_name;
    std::string canonical_name;
    std::string version = "unknown";
    std::string language = "en";
    std::string region = "unknown";
    bool supports_singleplayer = true;
    bool supports_multiplayer = true;
    std::uint8_t min_players = 1;
    std::uint8_t max_players = MaxPlayersPerClient;
    std::uint64_t updated_at = 0;
};

struct GameListRequest {
    std::uint64_t client_catalog_revision = 0;
};

struct GameList {
    std::uint64_t catalog_revision = 0;
    bool full = true;
    std::vector<GameInfo> games;
    std::vector<GameId> deleted_game_ids;
};

struct ActiveSessionInfoRequest {
};

struct ActiveSessionInfo {
    bool active = false;
    std::optional<GameId> selected_game_id;
    GameSessionMode session_mode = GameSessionMode::SinglePlayer;
    std::uint8_t player_count = 0;
    std::uint8_t connected_players = 0;
    std::uint8_t disconnected_players = 0;
    std::uint8_t viewer_count = 0;
    bool video_enabled = false;
    bool audio_enabled = false;
};

struct ControllerInfo {
    LocalPlayerIndex local_player = 0;
    std::string name;
    std::string guid;
    std::uint16_t vendor_id = 0;
    std::uint16_t product_id = 0;
};

struct ClientHello {
    std::string username;
    std::string display_name;
    std::optional<GameId> selected_game_id;
    GameSessionMode session_mode = GameSessionMode::SinglePlayer;
    std::uint8_t requested_players = 0;
    std::vector<ControllerInfo> controllers;
    bool wants_video = true;
    bool wants_audio = true;
};

struct HostWelcome {
    ClientId client_id = 0;
    std::uint8_t max_players_for_client = MaxPlayersPerClient;
    bool host_is_player = false;
};

struct ClientConfig {
    std::optional<std::string> username;
    std::optional<std::string> display_name;
    std::optional<GameId> selected_game_id;
    GameSessionMode session_mode = GameSessionMode::SinglePlayer;
    std::uint8_t requested_players = 0;
    std::vector<ControllerInfo> controllers;
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

struct SessionReady {
    GameId selected_game_id;
    GameSessionMode session_mode = GameSessionMode::SinglePlayer;
    std::uint8_t player_count = 0;
};

struct SessionStarting {
    GameId selected_game_id;
    GameSessionMode session_mode = GameSessionMode::SinglePlayer;
    std::uint8_t player_count = 0;
};

struct SessionEnded {
    std::string reason;
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

struct ArtAssetRequest {
    std::string asset_key;
    std::string role; // boxart, grid, hero, logo, icon, screenshot
};

struct ArtAssetResponse {
    std::string asset_key;
    std::string role;
    bool found = false;
    std::string extension; // e.g. ".png"
    std::vector<std::uint8_t> data;
};

using PacketPayload = std::variant<
    ClientHello,
    HostWelcome,
    ClientConfig,
    SeatAssignment,
    ControllerInput,
    ViewerHeartbeat,
    ErrorPacket,
    GameListRequest,
    GameList,
    ActiveSessionInfoRequest,
    ActiveSessionInfo,
    SessionReady,
    SessionStarting,
    SessionEnded,
    MediaEndpoint,
    ArtAssetRequest,
    ArtAssetResponse>;

ClientRole role_for_player_count(std::uint8_t requested_players);
bool valid_player_count(std::uint8_t requested_players);
bool valid_controller_info_count(std::size_t count);
bool valid_game_player_limits(std::uint8_t min_players, std::uint8_t max_players);
bool valid_username(std::string_view username);

} // namespace archstreamer
