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
constexpr std::uint16_t ProtocolVersion = 27;
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
    // Host → client: warm a second video RTP receive path (quality cutover).
    MediaVideoPending = 26,
    // Client → host: staging video path is receiving; host may promote + tear down old.
    MediaVideoReady = 27,
    // Client → host: explicit pause / fast-forward (not toggle or hold).
    EmulatorControl = 28,
    // Client → host: diagnostic log dump (side channel; also accepted as lone TCP payload).
    ClientLogBundle = 29,
    // Host → client: password ok but must be changed before join continues.
    PasswordChangeRequired = 30,
    // Client → host: set a new password (join handshake or Profile side-channel).
    PasswordChange = 31,
    // Client → host: normalized % within DS bottom screen (0..65535); host → DS pixels.
    TouchInput = 32,
    // Host → client: melonDS top/bottom screen AABBs in window pixels (follows swap).
    DsScreenLayout = 33,
    // Client → host: stay on the control socket after catalog (Users-tab Connected).
    LobbyPresence = 34,
    // Host → client: presence accepted (client keeps the TCP open until leave/play).
    LobbyPresenceAck = 35,
    // Client → host: pull profile controls.sqlite for username.
    ControlsDbPull = 36,
    // Host → client: controls DB bytes (empty db_bytes when missing).
    ControlsDbResponse = 37,
    // Client → host: push profile controls.sqlite for username.
    ControlsDbPush = 38,
    // Host → client: push result.
    ControlsDbAck = 39,
    /** Per-user blocked game ids after auth (shared catalog stays unfiltered). */
    CatalogUserBlocks = 40,
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
    // Trailing (protocol v21+) — omitted by older hosts → treat as unknown.
    std::optional<std::uint8_t> active_slots;
    std::optional<std::uint8_t> max_slots;
};

struct ControllerInfo {
    LocalPlayerIndex local_player = 0;
    std::string name;
    std::string guid;
    std::uint16_t vendor_id = 0;
    std::uint16_t product_id = 0;
};

/**
 * Client display geometry hint (trailing on Hello / ViewerHeartbeat).
 * For Nintendo DS (melonDS): Landscape → Hybrid Top, Portrait → Top/Bottom.
 * Auto → host default (Hybrid Top). Older peers omit the field (Auto).
 */
enum class DisplayLayoutPreference : std::uint8_t {
    Auto = 0,
    Landscape = 1,
    Portrait = 2,
};

inline const char* display_layout_preference_name(DisplayLayoutPreference value) {
    switch (value) {
    case DisplayLayoutPreference::Landscape:
        return "landscape";
    case DisplayLayoutPreference::Portrait:
        return "portrait";
    case DisplayLayoutPreference::Auto:
    default:
        return "auto";
    }
}

struct ClientHello {
    std::string username;
    std::string display_name;
    std::optional<GameId> selected_game_id;
    GameSessionMode session_mode = GameSessionMode::SinglePlayer;
    std::uint8_t requested_players = 0;
    std::vector<ControllerInfo> controllers;
    bool wants_video = true;
    bool wants_audio = true;
    // Trailing — older peers omit it (Auto).
    DisplayLayoutPreference display_layout = DisplayLayoutPreference::Auto;
    // Required at protocol v18 — host verifies / creates cadence user credentials.
    std::string password;
    // Trailing v27 — cached CatalogUserBlocks revision (0 = unknown / full send).
    std::uint64_t client_blocks_revision = 0;
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

/** Normalized touch within the DS bottom pane (client → host UDP).
 *  x/y are 0..65535 (= 0..1 of the bottom screen content). Host maps to DS
 *  stylus pixels 0–255 × 0–191 via ds_coords_from_normalized_u16 — the bottom
 *  framebuffer is always that size, so no host window rect is required. */
struct TouchInput {
    ClientId client_id = 0;
    LocalPlayerIndex local_player = 0;
    std::uint32_t sequence = 0;
    std::uint64_t timestamp_us = 0;
    std::uint16_t x = 0; // 0..DsTouchNormMax within bottom screen
    std::uint16_t y = 0; // 0..DsTouchNormMax within bottom screen
    bool pressed = false; // true = down/move, false = up
};

/**
 * Optional host → client: DS screen panes in melonDS window pixels.
 * Clients normally locate the bottom pane themselves (shared layout policy);
 * this packet is available if a client wants host-authoritative rects.
 */
struct DsScreenLayout {
    std::uint16_t window_w = 0;
    std::uint16_t window_h = 0;
    bool has_top = false;
    std::int16_t top_x = 0;
    std::int16_t top_y = 0;
    std::int16_t top_w = 0;
    std::int16_t top_h = 0;
    bool has_bot = false;
    std::int16_t bot_x = 0;
    std::int16_t bot_y = 0;
    std::int16_t bot_w = 0;
    std::int16_t bot_h = 0;

    bool operator==(const DsScreenLayout&) const = default;
};

/**
 * Explicit playback controls (client → host). Tri-state so a packet can change
 * pause, FF, both, or neither — absolute desired state, not a raw key toggle.
 *
 * Host expands these fields into ArchStreamer intents (see EmulatorControlPlane):
 * stateful Pause / FastForward; action kinds (ScreenSwap, …) via [action].
 */
enum class EmulatorControlState : std::uint8_t {
    Unchanged = 0,
    Off = 1,
    On = 2,
};

/** One-shot EmulatorControlPlane action codes (0 = none; ≥64 match EmulatorIntentKind). */
constexpr std::uint8_t EmulatorControlActionNone = 0;
constexpr std::uint8_t EmulatorControlActionScreenSwap = 64;

struct EmulatorControl {
    ClientId client_id = 0;
    EmulatorControlState pause = EmulatorControlState::Unchanged;
    EmulatorControlState fast_forward = EmulatorControlState::Unchanged;
    /** Non-zero: host re-applies even when its cache already matches (idempotent backends). */
    std::uint8_t force = 0;
    /** 0 = none; otherwise an EmulatorIntentKind action code (e.g. ScreenSwap=64). */
    std::uint8_t action = EmulatorControlActionNone;
};

enum class MediaQualityTier : std::uint8_t {
    Auto = 0,
    Low = 1,
    Medium = 2,
    High = 3,
    MediumHigh = 4,
    VeryHigh = 5,
};

/** Encode output height ladder (independent of bitrate/FPS quality). */
enum class MediaStreamSize : std::uint8_t {
    Auto = 0, // Client resolves from local screen height before send.
    P540 = 1,
    P720 = 2,
    P1080 = 3,
    P1440 = 4,
};

/**
 * Pre-encode queue depth + NVENC preset tradeoff (independent of size/quality).
 * Trailing on ViewerHeartbeat — older peers omit → LowLatency.
 */
enum class MediaStreamFeel : std::uint8_t {
    LowLatency = 0, // queue=1, nvenc hp (current default)
    Balanced = 1,   // queue=2, nvenc hp
    Smooth = 2,     // queue=4, nvenc hq (pre-d5edd11 smoother feel)
};

/**
 * Encode bitrate ladder (independent of framerate tier).
 * Trailing on ViewerHeartbeat — older peers omit → Auto (legacy combined tier).
 */
enum class MediaStreamBitrate : std::uint8_t {
    Auto = 0, // Use legacy bitrate from MediaQualityTier.
    Kbps800 = 1,
    Kbps3500 = 2,
    Kbps8000 = 3,
    Kbps12000 = 4,
    Kbps25000 = 5,
};

struct VideoEncodeSettings {
    std::uint16_t bitrate_kbps = 1500;
    std::uint8_t framerate = 30;
    std::uint16_t key_int_max = 30;
    // 0 = encode at capture resolution (no videoscale).
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    // Pre-encode queue depth (0 → treat as 1 in gst builder).
    std::uint8_t queue_buffers = 1;
    // Prefer NVENC low-latency-hq over low-latency-hp.
    bool nvenc_high_quality = false;

    friend bool operator==(const VideoEncodeSettings& a, const VideoEncodeSettings& b) {
        return a.bitrate_kbps == b.bitrate_kbps &&
            a.framerate == b.framerate &&
            a.key_int_max == b.key_int_max &&
            a.width == b.width &&
            a.height == b.height &&
            a.queue_buffers == b.queue_buffers &&
            a.nvenc_high_quality == b.nvenc_high_quality;
    }

    friend bool operator!=(const VideoEncodeSettings& a, const VideoEncodeSettings& b) {
        return !(a == b);
    }
};

/** Componentwise max for a single shared session encode (players-only ceiling). */
inline VideoEncodeSettings dominate_video_encode_settings(
    VideoEncodeSettings a,
    const VideoEncodeSettings& b) {
    if (b.width > a.width) {
        a.width = b.width;
    }
    if (b.height > a.height) {
        a.height = b.height;
    }
    if (b.framerate > a.framerate) {
        a.framerate = b.framerate;
    }
    if (b.bitrate_kbps > a.bitrate_kbps) {
        a.bitrate_kbps = b.bitrate_kbps;
    }
    if (b.key_int_max > a.key_int_max) {
        a.key_int_max = b.key_int_max;
    }
    if (b.queue_buffers > a.queue_buffers) {
        a.queue_buffers = b.queue_buffers;
    }
    a.nvenc_high_quality = a.nvenc_high_quality || b.nvenc_high_quality;
    return a;
}

inline MediaStreamFeel media_stream_feel_for_settings(const VideoEncodeSettings& settings) {
    if (settings.queue_buffers >= 4 || settings.nvenc_high_quality) {
        return MediaStreamFeel::Smooth;
    }
    if (settings.queue_buffers >= 2) {
        return MediaStreamFeel::Balanced;
    }
    return MediaStreamFeel::LowLatency;
}

/** Nearest explicit bitrate enum (never Auto) for applied_* mirrors. */
inline MediaStreamBitrate media_stream_bitrate_for_settings(const VideoEncodeSettings& settings) {
    if (settings.bitrate_kbps >= 18000) {
        return MediaStreamBitrate::Kbps25000;
    }
    if (settings.bitrate_kbps >= 10000) {
        return MediaStreamBitrate::Kbps12000;
    }
    if (settings.bitrate_kbps >= 6000) {
        return MediaStreamBitrate::Kbps8000;
    }
    if (settings.bitrate_kbps <= 1000) {
        return MediaStreamBitrate::Kbps800;
    }
    return MediaStreamBitrate::Kbps3500;
}

inline std::uint16_t media_stream_size_height(MediaStreamSize size) {
    switch (size) {
    case MediaStreamSize::P540:
        return 540;
    case MediaStreamSize::P1080:
        return 1080;
    case MediaStreamSize::P1440:
        return 1440;
    case MediaStreamSize::P720:
    case MediaStreamSize::Auto:
    default:
        return 720;
    }
}

/** Pick a concrete size from local display height (client-side Auto resolution). */
inline MediaStreamSize media_stream_size_for_display_height(int display_height) {
    if (display_height >= 1440) {
        return MediaStreamSize::P1440;
    }
    if (display_height >= 1080) {
        return MediaStreamSize::P1080;
    }
    if (display_height >= 720) {
        return MediaStreamSize::P720;
    }
    return MediaStreamSize::P540;
}

/** Compat: old combined-tier → size used before the split. */
inline MediaStreamSize media_stream_size_for_legacy_tier(MediaQualityTier tier) {
    switch (tier) {
    case MediaQualityTier::Low:
        return MediaStreamSize::P540;
    case MediaQualityTier::High:
    case MediaQualityTier::VeryHigh:
        return MediaStreamSize::P1080;
    case MediaQualityTier::MediumHigh:
    case MediaQualityTier::Medium:
    case MediaQualityTier::Auto:
    default:
        return MediaStreamSize::P720;
    }
}

inline VideoEncodeSettings video_encode_settings_for_quality(MediaQualityTier quality) {
    // key_int_max ~0.5s so remotes get an IDR quickly after join / pipeline restart.
    // Legacy combined fps+bitrate ladder (used when MediaStreamBitrate::Auto).
    switch (quality) {
    case MediaQualityTier::Low:
        return VideoEncodeSettings{800, 20, 10, 0, 0};
    case MediaQualityTier::MediumHigh:
        return VideoEncodeSettings{8000, 60, 30, 0, 0};
    case MediaQualityTier::High:
        return VideoEncodeSettings{12000, 60, 30, 0, 0};
    case MediaQualityTier::VeryHigh:
        return VideoEncodeSettings{25000, 60, 30, 0, 0};
    case MediaQualityTier::Medium:
    case MediaQualityTier::Auto:
    default:
        return VideoEncodeSettings{3500, 30, 15, 0, 0};
    }
}

/** Frame rate from wanted_tier (UI Frame rate); Med-High/Very-High → 60. */
inline std::uint8_t framerate_for_quality_tier(MediaQualityTier quality) {
    switch (quality) {
    case MediaQualityTier::Low:
        return 20;
    case MediaQualityTier::High:
    case MediaQualityTier::MediumHigh:
    case MediaQualityTier::VeryHigh:
        return 60;
    case MediaQualityTier::Medium:
    case MediaQualityTier::Auto:
    default:
        return 30;
    }
}

inline std::uint16_t key_int_max_for_framerate(std::uint8_t framerate) {
    if (framerate <= 20) {
        return 10;
    }
    if (framerate >= 50) {
        return 30;
    }
    return 15;
}

inline std::uint16_t bitrate_kbps_for_stream_bitrate(MediaStreamBitrate bitrate) {
    switch (bitrate) {
    case MediaStreamBitrate::Kbps800:
        return 800;
    case MediaStreamBitrate::Kbps8000:
        return 8000;
    case MediaStreamBitrate::Kbps12000:
        return 12000;
    case MediaStreamBitrate::Kbps25000:
        return 25000;
    case MediaStreamBitrate::Kbps3500:
    case MediaStreamBitrate::Auto:
    default:
        return 3500;
    }
}

/** Default bitrate when Bitrate=Auto but fps is taken from an explicit frame-rate tier. */
inline MediaStreamBitrate default_bitrate_for_framerate_tier(MediaQualityTier quality) {
    switch (quality) {
    case MediaQualityTier::Low:
        return MediaStreamBitrate::Kbps800;
    case MediaQualityTier::MediumHigh:
        return MediaStreamBitrate::Kbps8000;
    case MediaQualityTier::High:
        return MediaStreamBitrate::Kbps12000;
    case MediaQualityTier::VeryHigh:
        return MediaStreamBitrate::Kbps25000;
    case MediaQualityTier::Medium:
    case MediaQualityTier::Auto:
    default:
        return MediaStreamBitrate::Kbps3500;
    }
}

inline void apply_media_stream_feel(VideoEncodeSettings& settings, MediaStreamFeel feel) {
    switch (feel) {
    case MediaStreamFeel::Balanced:
        settings.queue_buffers = 2;
        settings.nvenc_high_quality = false;
        break;
    case MediaStreamFeel::Smooth:
        settings.queue_buffers = 4;
        settings.nvenc_high_quality = true;
        break;
    case MediaStreamFeel::LowLatency:
    default:
        settings.queue_buffers = 1;
        settings.nvenc_high_quality = false;
        break;
    }
}

inline VideoEncodeSettings video_encode_settings(
    MediaStreamSize size,
    MediaQualityTier quality,
    std::uint16_t capture_width = 1920,
    std::uint16_t capture_height = 1080,
    MediaStreamFeel feel = MediaStreamFeel::LowLatency,
    MediaStreamBitrate bitrate = MediaStreamBitrate::Auto) {
    VideoEncodeSettings settings;
    if (bitrate == MediaStreamBitrate::Auto) {
        // Older clients / Auto bitrate: full legacy fps+bitrate from tier.
        settings = video_encode_settings_for_quality(quality);
    } else {
        const auto fps = framerate_for_quality_tier(quality);
        settings.framerate = fps;
        settings.key_int_max = key_int_max_for_framerate(fps);
        settings.bitrate_kbps = bitrate_kbps_for_stream_bitrate(bitrate);
    }
    if (size == MediaStreamSize::Auto) {
        size = MediaStreamSize::P720;
    }
    if (capture_width == 0) {
        capture_width = 1920;
    }
    if (capture_height == 0) {
        capture_height = 1080;
    }

    std::uint16_t height = media_stream_size_height(size);
    if (height > capture_height) {
        height = capture_height;
    }
    // Preserve capture aspect; fall back to 16:9 if capture is nonsense.
    std::uint32_t width =
        (static_cast<std::uint32_t>(height) * capture_width + capture_height / 2) /
        capture_height;
    if (width < 2) {
        width = (static_cast<std::uint32_t>(height) * 16) / 9;
    }
    width &= ~1u; // H.264-friendly even width.
    height = static_cast<std::uint16_t>(height & ~1u);
    settings.width = static_cast<std::uint16_t>(width);
    settings.height = height;
    apply_media_stream_feel(settings, feel);
    return settings;
}

/** Legacy helper: combined tier maps to historical size+quality pairing. */
inline VideoEncodeSettings video_encode_settings_for_tier(MediaQualityTier tier) {
    return video_encode_settings(media_stream_size_for_legacy_tier(tier), tier);
}

// Map encode settings back to a quality ladder tier (bitrate/FPS only).
inline MediaQualityTier media_quality_tier_for_settings(const VideoEncodeSettings& settings) {
    if (settings.bitrate_kbps >= 18000) {
        return MediaQualityTier::VeryHigh;
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

inline MediaStreamSize media_stream_size_for_settings(const VideoEncodeSettings& settings) {
    if (settings.height >= 1440) {
        return MediaStreamSize::P1440;
    }
    if (settings.height >= 1080) {
        return MediaStreamSize::P1080;
    }
    if (settings.height >= 720) {
        return MediaStreamSize::P720;
    }
    if (settings.height > 0) {
        return MediaStreamSize::P540;
    }
    return MediaStreamSize::P720;
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
           video_encode_settings_for_quality(tier).bitrate_kbps > max_bitrate_kbps) {
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

inline const char* media_stream_size_name(MediaStreamSize size) {
    switch (size) {
    case MediaStreamSize::Auto:
        return "auto";
    case MediaStreamSize::P540:
        return "540p";
    case MediaStreamSize::P720:
        return "720p";
    case MediaStreamSize::P1080:
        return "1080p";
    case MediaStreamSize::P1440:
        return "1440p";
    }
    return "unknown";
}

inline const char* media_stream_feel_name(MediaStreamFeel feel) {
    switch (feel) {
    case MediaStreamFeel::LowLatency:
        return "low-latency";
    case MediaStreamFeel::Balanced:
        return "balanced";
    case MediaStreamFeel::Smooth:
        return "smooth";
    }
    return "unknown";
}

inline const char* media_stream_bitrate_name(MediaStreamBitrate bitrate) {
    switch (bitrate) {
    case MediaStreamBitrate::Auto:
        return "auto";
    case MediaStreamBitrate::Kbps800:
        return "0.8Mbps";
    case MediaStreamBitrate::Kbps3500:
        return "3.5Mbps";
    case MediaStreamBitrate::Kbps8000:
        return "8Mbps";
    case MediaStreamBitrate::Kbps12000:
        return "12Mbps";
    case MediaStreamBitrate::Kbps25000:
        return "25Mbps";
    }
    return "unknown";
}

/** Framerate-only Auto ladder when bitrate is fixed (skip Med-High/Very-High aliases). */
inline MediaQualityTier step_framerate_tier_down(MediaQualityTier tier) {
    switch (tier) {
    case MediaQualityTier::VeryHigh:
    case MediaQualityTier::High:
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

inline MediaQualityTier step_framerate_tier_up(MediaQualityTier tier) {
    switch (tier) {
    case MediaQualityTier::Low:
        return MediaQualityTier::Medium;
    case MediaQualityTier::Medium:
    case MediaQualityTier::Auto:
        return MediaQualityTier::High;
    case MediaQualityTier::MediumHigh:
    case MediaQualityTier::High:
    case MediaQualityTier::VeryHigh:
    default:
        return MediaQualityTier::High;
    }
}

/** Parse "WxH" capture resolution; returns false and leaves outs unchanged on failure. */
inline bool parse_video_resolution(
    std::string_view text,
    std::uint16_t& width,
    std::uint16_t& height) {
    const auto x = text.find('x');
    if (x == std::string_view::npos || x == 0 || x + 1 >= text.size()) {
        return false;
    }
    try {
        const auto w = std::stoi(std::string(text.substr(0, x)));
        const auto h = std::stoi(std::string(text.substr(x + 1)));
        if (w < 2 || h < 2 || w > 7680 || h > 4320) {
            return false;
        }
        width = static_cast<std::uint16_t>(w);
        height = static_cast<std::uint16_t>(h);
        return true;
    } catch (const std::exception&) {
        return false;
    }
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
    // Encode height ladder (independent of wanted_tier). Trailing — older peers omit
    // it and it stays Auto (host then keeps applied size / legacy Medium→720p).
    MediaStreamSize wanted_size = MediaStreamSize::Auto;
    // Trailing — older peers omit (Auto). DS host uses Landscape→Hybrid, Portrait→Top/Bottom.
    DisplayLayoutPreference display_layout = DisplayLayoutPreference::Auto;
    // Trailing — older peers omit → LowLatency (current encode defaults).
    MediaStreamFeel wanted_feel = MediaStreamFeel::LowLatency;
    // Trailing — older peers omit → Auto (legacy combined fps+bitrate from wanted_tier).
    MediaStreamBitrate wanted_bitrate = MediaStreamBitrate::Auto;
};

struct ErrorPacket {
    std::string message;
};

struct ArtAssetRequest {
    std::string asset_key;
    std::string role; // boxart, grid, hero, logo, icon, screenshot
    // Trailing — older peers omit. Client's cached content hash (sha256:…); empty = unknown.
    std::string cached_sha256;
};

struct ArtAssetResponse {
    std::string asset_key;
    std::string role;
    bool found = false;
    std::string extension; // e.g. ".png"
    std::vector<std::uint8_t> data;
    // Trailing — older peers omit. Host file hash; when found and data empty, cache is current.
    std::string content_sha256;
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
// request_id == 0: unsolicited / manual pad OSK — host should find a dialog and inject.
struct SoftKeyboardResponse {
    std::uint32_t request_id = 0;
    bool accepted = false;
    std::string text;
};

// Host → client: open a second video receive path; do not touch audio.
struct MediaVideoPending {
    std::string video_uri;
};

// Client → host: staging URI is up; echo uri so host can match in-flight cutovers.
// Empty video_uri is a client NACK (staging bind/decode failed) — host aborts cutover.
struct MediaVideoReady {
    std::string video_uri;
};

// Client → host: UTF-8 diagnostic log text covering the last N app sessions.
struct ClientLogBundle {
    std::string username;
    std::uint32_t session_count = 0;
    std::vector<std::uint8_t> text;
};

// Host → client: empty; client must send PasswordChange on the same socket.
struct PasswordChangeRequired {};

// Client → host: change password (forced join change or Profile anytime).
struct PasswordChange {
    std::string username;
    std::string current_password;
    std::string new_password;
};

/**
 * Client → host after GameList: announce catalog presence and keep the TCP open.
 * Host shows Users-tab Status=Connected; Kick closes this socket (not a blacklist).
 */
struct LobbyPresence {
    std::string username;
    std::string password;
    // Trailing v27 — cached CatalogUserBlocks revision (0 = unknown / full send).
    std::uint64_t client_blocks_revision = 0;
};

/** Host → client: presence registered; keep the control connection open. */
struct LobbyPresenceAck {
    ClientId client_id = 0;
};

/** Client → host: download <save_root>/<username>/controls.sqlite. */
struct ControlsDbPull {
    std::string username;
};

/** Host → client: found=false and empty bytes when the profile has no controls DB yet. */
struct ControlsDbResponse {
    std::string username;
    bool found = false;
    std::vector<std::uint8_t> db_bytes;
};

/** Client → host: replace profile controls.sqlite (must only contain this username). */
struct ControlsDbPush {
    std::string username;
    std::vector<std::uint8_t> db_bytes;
};

/** Host → client: push accepted or rejected. */
struct ControlsDbAck {
    std::string username;
    bool ok = false;
    std::string message;
};

/** Host → client after LobbyPresence / ClientHello auth: titles this user cannot play. */
struct CatalogUserBlocks {
    std::uint64_t blocks_revision = 0;
    bool full = true;
    std::vector<GameId> blocked_game_ids;
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
    MediaVideoPending,
    MediaVideoReady,
    KeyboardInput,
    TouchInput,
    DsScreenLayout,
    EmulatorControl,
    ClientLogBundle,
    PasswordChangeRequired,
    PasswordChange,
    LobbyPresence,
    LobbyPresenceAck,
    ControlsDbPull,
    ControlsDbResponse,
    ControlsDbPush,
    ControlsDbAck,
    CatalogUserBlocks>;

ClientRole role_for_player_count(std::uint8_t requested_players);
bool valid_player_count(std::uint8_t requested_players);
bool valid_controller_info_count(std::size_t count);
bool valid_game_player_limits(std::uint8_t min_players, std::uint8_t max_players);
bool valid_username(std::string_view username);

} // namespace archstreamer
