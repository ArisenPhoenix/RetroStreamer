#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace archstreamer::cadence {

/**
 * Persisted user identity + credentials for the control plane.
 * Shared by host, GUI, and any platform that talks to cadence (file or db).
 * Save-game blobs stay under the save root; this is auth + lookup only.
 */
struct UserRecord {
    std::string username;
    std::string display_name;
    /**
     * Salted password hash (`v1:…`) or, briefly during migration, legacy plaintext.
     * Never log this field.
     */
    std::string password_hash;
    bool must_change = false;
    /** Unix epoch seconds when the row was first created; 0 = unknown. */
    std::int64_t created_at = 0;
    /** Unix epoch seconds when last upserted. */
    std::int64_t updated_at = 0;
};

/**
 * One live (or recently ended) play session. Resource claims hang off session_id.
 * Used for handoff / availability: what does this session still hold?
 */
struct SessionRecord {
    std::string session_id;
    std::string host_id;
    int slot = -1;
    std::string username;
    std::string game_key;
    std::string system_key;
    std::string mode;
    std::int64_t started_at = 0;
    /** 0 = still active. */
    std::int64_t ended_at = 0;
    std::string end_reason;
};

/**
 * A shared machine resource held by a session (sink, display, port, pid, …).
 * Primary lookup key is (resource_type, resource_name); at most one held claim.
 */
struct ResourceClaim {
    std::string session_id;
    std::string resource_type;
    std::string resource_name;
    std::string host_id;
    int slot = -1;
    std::int64_t claimed_at = 0;
    /** 0 = currently held. */
    std::int64_t released_at = 0;
    std::string detail;
};

namespace resource {
inline constexpr std::string_view kSlotLock = "slot_lock";
inline constexpr std::string_view kPulseSink = "pulse_sink";
inline constexpr std::string_view kPulseAppId = "pulse_app_id";
inline constexpr std::string_view kDisplay = "display";
inline constexpr std::string_view kVideoPort = "video_port";
inline constexpr std::string_view kAudioPort = "audio_port";
inline constexpr std::string_view kNetcmdPort = "netcmd_port";
inline constexpr std::string_view kEmulatorPid = "emulator_pid";
inline constexpr std::string_view kPadProductBase = "pad_product_base";
} // namespace resource

/**
 * Structured control-plane event. Not for pad/media hot path.
 * Day partitioning uses local calendar date of `timestamp`.
 */
struct RuntimeEvent {
    /** Unix epoch seconds; 0 = store fills "now". */
    std::int64_t timestamp = 0;
    std::string kind;
    std::string host_id;
    int slot = -1;
    std::string username;
    std::string game_key;
    std::string detail;
    /** Empty when the event is not tied to a tracked session. */
    std::string session_id;
};

/** YYYY-MM-DD in local time for day tables / event files. */
std::string day_string_from_epoch(std::int64_t epoch_seconds);

std::int64_t now_epoch_seconds();

} // namespace archstreamer::cadence
