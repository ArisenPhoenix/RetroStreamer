#pragma once

#include "common/controller_state.hpp"
#include "common/keyboard_state.hpp"
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
constexpr std::uint16_t ProtocolVersion = 14;
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
    DiscControlRequest = 18,
    DiscControlResponse = 19,
    KeyboardInput = 20,
    // Client → host: intentional Stop / video-window close (not a link drop).
    ClientSessionLeave = 21,
    LinkRequest = 22,
    LinkResponse = 23,
    // Host → client: Ryujinx (or similar) Software Keyboard needs text from the pad OSK.
    SoftKeyboardRequest = 24,
    // Client → host: typed text (or cancel) for SoftKeyboardRequest.
    SoftKeyboardResponse = 25,
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
    // Multi-disc playlist labels (from .m3u); empty when not a playlist game.
    std::vector<std::string> playlist_discs;
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

// Client tells the host it is leaving on purpose (Stop Client / close video window).
// Host ends the session immediately; TCP drops without this get the reconnect window.
struct ClientSessionLeave {
    ClientId client_id = 0;
    std::string reason;
};

struct ControllerInput {
    ClientId client_id = 0;
    LocalPlayerIndex local_player = 0;
    ControllerState state;
};

struct KeyboardInput {
    ClientId client_id = 0;
    LocalPlayerIndex local_player = 0;
    KeyboardState state;
};

enum class MediaQualityTier : std::uint8_t {
    Auto = 0,
    Low = 1,
    Medium = 2,
    High = 3,
    MediumHigh = 4,
    VeryHigh = 5,
};

struct VideoEncodeSettings {
    std::uint16_t bitrate_kbps = 1500;
    std::uint8_t framerate = 30;
    std::uint16_t key_int_max = 30;
    // 0 = encode at capture resolution (no videoscale).
    std::uint16_t width = 0;
    std::uint16_t height = 0;
};

inline VideoEncodeSettings video_encode_settings_for_tier(MediaQualityTier tier) {
    // key_int_max ~0.5s so remotes get an IDR quickly after join / pipeline restart.
    // Capture defaults to 1080p; lower tiers downscale in the encode branch.
    switch (tier) {
    case MediaQualityTier::Low:
        return VideoEncodeSettings{800, 20, 10, 960, 540};
    case MediaQualityTier::MediumHigh:
        return VideoEncodeSettings{8000, 60, 30, 1280, 720};
    case MediaQualityTier::High:
        return VideoEncodeSettings{12000, 60, 30, 1920, 1080};
    case MediaQualityTier::VeryHigh:
        return VideoEncodeSettings{25000, 60, 30, 1920, 1080};
    case MediaQualityTier::Medium:
    case MediaQualityTier::Auto:
    default:
        return VideoEncodeSettings{3500, 30, 15, 1280, 720};
    }
}

// Map encode settings back to a ladder tier (used when clients reconfigure by settings).
inline MediaQualityTier media_quality_tier_for_settings(const VideoEncodeSettings& settings) {
    if (settings.bitrate_kbps >= 18000 || settings.width >= 1920) {
        if (settings.bitrate_kbps >= 18000) {
            return MediaQualityTier::VeryHigh;
        }
        return MediaQualityTier::High;
    }
    if (settings.bitrate_kbps >= 10000) {
        return MediaQualityTier::High;
    }
    if (settings.bitrate_kbps >= 6000 || settings.framerate >= 50) {
        return MediaQualityTier::MediumHigh;
    }
    if (settings.framerate <= 20 || settings.bitrate_kbps <= 1000) {
        return MediaQualityTier::Low;
    }
    return MediaQualityTier::Medium;
}

inline MediaQualityTier step_quality_tier_down(MediaQualityTier tier) {
    switch (tier) {
    case MediaQualityTier::VeryHigh:
        return MediaQualityTier::High;
    case MediaQualityTier::High:
        return MediaQualityTier::MediumHigh;
    case MediaQualityTier::MediumHigh:
        return MediaQualityTier::Medium;
    case MediaQualityTier::Medium:
    case MediaQualityTier::Auto:
        return MediaQualityTier::Low;
    case MediaQualityTier::Low:
    default:
        return MediaQualityTier::Low;
    }
}

inline MediaQualityTier step_quality_tier_up(MediaQualityTier tier) {
    switch (tier) {
    case MediaQualityTier::Low:
        return MediaQualityTier::Medium;
    case MediaQualityTier::Medium:
    case MediaQualityTier::Auto:
        return MediaQualityTier::MediumHigh;
    case MediaQualityTier::MediumHigh:
        return MediaQualityTier::High;
    case MediaQualityTier::High:
        return MediaQualityTier::VeryHigh;
    case MediaQualityTier::VeryHigh:
    default:
        return MediaQualityTier::VeryHigh;
    }
}

// Host-side selector: resolve the tier a client should receive on the encode ladder.
// - wanted == Auto → use auto_tier (adaptation state)
// - otherwise → wanted, optionally capped by max_bitrate_kbps
inline MediaQualityTier select_video_tier(
    MediaQualityTier wanted,
    MediaQualityTier auto_tier,
    std::uint16_t max_bitrate_kbps = 0) {
    MediaQualityTier tier = wanted == MediaQualityTier::Auto ? auto_tier : wanted;
    if (tier == MediaQualityTier::Auto) {
        tier = MediaQualityTier::Medium;
    }
    if (max_bitrate_kbps == 0) {
        return tier;
    }
    while (tier != MediaQualityTier::Low &&
           video_encode_settings_for_tier(tier).bitrate_kbps > max_bitrate_kbps) {
        tier = step_quality_tier_down(tier);
    }
    return tier;
}

inline const char* media_quality_tier_name(MediaQualityTier tier) {
    switch (tier) {
    case MediaQualityTier::Auto:
        return "auto";
    case MediaQualityTier::Low:
        return "low";
    case MediaQualityTier::Medium:
        return "medium";
    case MediaQualityTier::MediumHigh:
        return "med-high";
    case MediaQualityTier::High:
        return "high";
    case MediaQualityTier::VeryHigh:
        return "very-high";
    }
    return "unknown";
}

struct ViewerHeartbeat {
    ClientId client_id = 0;
    std::uint32_t sequence = 0;
    // 0–1000: estimated RTP/media loss in tenths of a percent (best-effort).
    std::uint16_t loss_permille = 0;
    // Decoded video frames since the previous heartbeat (0 if no video / stalled).
    std::uint16_t frames_decoded_delta = 0;
    MediaQualityTier wanted_tier = MediaQualityTier::Auto;
    // 0 = use tier default bitrate cap.
    std::uint16_t max_bitrate_kbps = 0;
    // Ask host to overlay a ticking RetroArch "Frames:" counter (debug; default off).
    // Trailing field — older peers omit it and it stays false.
    bool show_framecount = false;
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

enum class DiscControlAction : std::uint8_t {
    SetIndex = 0,
    Next = 1,
    Prev = 2,
};

struct DiscControlRequest {
    GameId game_id;
    DiscControlAction action = DiscControlAction::SetIndex;
    // Used when action == SetIndex (0-based playlist index).
    std::uint8_t disc_index = 0;
};

struct DiscControlResponse {
    bool ok = false;
    std::uint8_t disc_index = 0;
    std::uint8_t disc_count = 0;
    std::string message;
};

enum class LinkAction : std::uint8_t {
    Request = 0,
    Cancel = 1,
};

enum class LinkStatus : std::uint8_t {
    Pending = 0,
    Matched = 1,
    Cancelled = 2,
    Error = 3,
};

// Client → host: ask to link with another seated username (mutual handshake).
struct LinkRequest {
    GameId game_id;
    std::string target_username;
    LinkAction action = LinkAction::Request;
};

struct LinkResponse {
    bool ok = false;
    LinkStatus status = LinkStatus::Error;
    std::string peer_username;
    std::string message;
};

// Host asks the client GUI to open the pad on-screen keyboard.
struct SoftKeyboardRequest {
    std::uint32_t request_id = 0;
    std::string prompt;
    std::string initial_text;
    std::uint8_t max_length = 12;
};

// Client returns the pad-OSK result (accepted=false → cancelled).
struct SoftKeyboardResponse {
    std::uint32_t request_id = 0;
    bool accepted = false;
    std::string text;
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
    ClientSessionLeave,
    MediaEndpoint,
    ArtAssetRequest,
    ArtAssetResponse,
    DiscControlRequest,
    DiscControlResponse,
    LinkRequest,
    LinkResponse,
    SoftKeyboardRequest,
    SoftKeyboardResponse,
    KeyboardInput>;

ClientRole role_for_player_count(std::uint8_t requested_players);
bool valid_player_count(std::uint8_t requested_players);
bool valid_controller_info_count(std::size_t count);
bool valid_game_player_limits(std::uint8_t min_players, std::uint8_t max_players);
bool valid_username(std::string_view username);

} // namespace archstreamer
