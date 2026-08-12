#pragma once

#include "common/protocol.hpp"
#include "host/session/lobby.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace archstreamer {

struct StreamAdaptationDecision {
    MediaStreamSize size = MediaStreamSize::Auto;
    MediaQualityTier tier = MediaQualityTier::Auto;
    MediaStreamFeel feel = MediaStreamFeel::LowLatency;
    MediaStreamBitrate bitrate = MediaStreamBitrate::Auto;
    MediaStreamFps fps = MediaStreamFps::Auto;
    std::string reason;
    std::string log_line;
};

struct StreamRequest {
    MediaStreamSize size = MediaStreamSize::Auto;
    MediaQualityTier tier = MediaQualityTier::Auto;
    MediaStreamFeel feel = MediaStreamFeel::LowLatency;
    MediaStreamBitrate bitrate = MediaStreamBitrate::Auto;
    MediaStreamFps fps = MediaStreamFps::Auto;
};

/** Snapshot of receive/decode health used when resolving a stream request. */
struct StreamClientHealthMetrics {
    std::uint16_t loss_permille = 0;
    std::uint16_t frames_decoded_delta = 0;
    std::uint16_t decode_queue_p95_ms = ViewerHeartbeatLatencyUnknownMs;
    std::uint16_t decode_queue_max_ms = ViewerHeartbeatLatencyUnknownMs;
    std::uint16_t au_queue_p95_ms = ViewerHeartbeatLatencyUnknownMs;
};

struct StreamRequestResolution {
    StreamRequest request{};
    /** True when health prevented a raise or stepped quality down. */
    bool limited_by_metrics = false;
    std::string reason;
};

StreamRequest stream_request_from_applied(const SessionClientConnection& client);
StreamClientHealthMetrics stream_health_metrics_from_client(const SessionClientConnection& client);
StreamClientHealthMetrics stream_health_metrics_from_heartbeat(const ViewerHeartbeat& heartbeat);

/**
 * Decide the effective stream request for a client before fanout planning.
 * Inputs: what they currently receive, what they newly asked for, and health.
 * May update adaptive_fps_cap / decode_pressure_streak on [client].
 */
StreamRequestResolution resolve_stream_request_for_client(
    SessionClientConnection& client,
    const StreamRequest& current,
    const StreamRequest& requested,
    const StreamClientHealthMetrics& health);

/**
 * Central host decision for what to do when a seated player wants stream settings.
 * Viewers never drive this path; callers should not invoke it for them (returns None).
 *
 * ReplaceTrunk = warm a new encode then hand off (target: session-wide trunk swap).
 * HardRestartTrunk = tear down / restart the shared encode immediately.
 * SyncToTrunk = ceiling unchanged; keep riding the existing trunk.
 * SampleFromTrunk = ceiling unchanged but this receiver needs a lighter derived stream
 *   (reserved; not emitted until weak-receiver sampling is wired).
 */
enum class StreamPipelineAction : std::uint8_t {
    None = 0,
    SyncToTrunk = 1,
    SampleFromTrunk = 2,
    ReplaceTrunk = 3,
    HardRestartTrunk = 4,
};

const char* stream_pipeline_action_name(StreamPipelineAction action);

StreamPipelineAction decide_stream_pipeline_action(
    const SessionClientConnection& client,
    bool is_seated_player,
    bool session_video_configured,
    const VideoEncodeSettings& current_trunk,
    const VideoEncodeSettings& proposed_trunk,
    bool cutover_in_flight,
    bool within_reconfigure_cooldown);

/**
 * Per-client fanout branch relative to the session trunk encode.
 *
 * TakeTrunk — receive the trunk bitstream directly (multiudpsink on the trunk).
 * SampleFromTrunk — needs a lighter derived stream at [settings] (new sample branch).
 * ReuseClient — same decided settings as an existing branch owner; share that stream
 *   (reuse_client_id is the TakeTrunk/SampleFromTrunk owner, never another ReuseClient).
 */
enum class StreamBranchAction : std::uint8_t {
    TakeTrunk = 0,
    SampleFromTrunk = 1,
    ReuseClient = 2,
};

struct StreamBranchCandidate {
    ClientId client_id = 0;
    bool connected = false;
    bool wants_video = false;
    /** Settings this client should receive (caller resolves weak-receiver caps). */
    VideoEncodeSettings target_settings{};
};

struct StreamBranchDecision {
    ClientId client_id = 0;
    StreamBranchAction action = StreamBranchAction::TakeTrunk;
    /** Effective receive settings (trunk copy or sample profile). */
    VideoEncodeSettings settings{};
    /** Valid when action == ReuseClient. */
    ClientId reuse_client_id = 0;
};

const char* stream_branch_action_name(StreamBranchAction action);

/** True when [target] matches the trunk encode (client can ride the trunk). */
bool stream_branch_matches_trunk(
    const VideoEncodeSettings& target,
    const VideoEncodeSettings& trunk);

/**
 * Decide one client's branch given the trunk and decisions already made for
 * earlier clients (used to find a reuse owner with matching settings).
 */
StreamBranchDecision decide_stream_branch_action(
    const StreamBranchCandidate& client,
    const VideoEncodeSettings& trunk,
    const std::vector<StreamBranchDecision>& prior_decisions);

/**
 * Decide branches for every video-wanting connected client in [clients] order.
 * First client with a given sample profile owns SampleFromTrunk; later matches Reuse.
 */
std::vector<StreamBranchDecision> decide_stream_branch_actions(
    const std::vector<StreamBranchCandidate>& clients,
    const VideoEncodeSettings& trunk);

/**
 * Unique RTP/encode stream after client consolidation.
 * TakeTrunk groups share the trunk encode; Sample groups share one sample encode.
 * ReuseClient members are folded into the owner's group.
 */
struct StreamBranchGroup {
    VideoEncodeSettings settings{};
    bool is_trunk = false;
    /** Sample owner (first SampleFromTrunk client). 0 when is_trunk. */
    ClientId owner_client_id = 0;
    std::vector<ClientId> member_client_ids;
};

/** One client's attach step when migrating a stream group. */
struct StreamClientMigration {
    ClientId client_id = 0;
    StreamBranchAction action = StreamBranchAction::TakeTrunk;
    VideoEncodeSettings settings{};
    /** Sample owner when action is ReuseClient; 0 for trunk / sample owner. */
    ClientId stream_owner_client_id = 0;
};

struct StreamFanoutPlan {
    VideoEncodeSettings current_trunk{};
    VideoEncodeSettings proposed_trunk{};
    MediaStreamSize proposed_size = MediaStreamSize::P720;
    MediaQualityTier proposed_tier = MediaQualityTier::Medium;
    MediaStreamFeel proposed_feel = MediaStreamFeel::LowLatency;
    MediaStreamBitrate proposed_bitrate = MediaStreamBitrate::Auto;
    MediaStreamFps proposed_fps = MediaStreamFps::Fps30;
    /** Trunk encode must change (proposed != current). */
    bool trunk_changes = false;
    StreamPipelineAction trunk_action = StreamPipelineAction::None;
    std::vector<StreamBranchDecision> branches;
    /** Unique streams after consolidation (iterate these for trunk handoff). */
    std::vector<StreamBranchGroup> streams;
};

std::vector<StreamBranchGroup> group_stream_branch_decisions(
    const std::vector<StreamBranchDecision>& decisions,
    const VideoEncodeSettings& trunk);

std::vector<StreamClientMigration> migrations_for_stream(const StreamBranchGroup& stream);

struct SessionVideoCeiling {
    VideoEncodeSettings settings{};
    MediaStreamSize size = MediaStreamSize::P720;
    MediaQualityTier tier = MediaQualityTier::Medium;
    MediaStreamFeel feel = MediaStreamFeel::LowLatency;
    MediaStreamBitrate bitrate = MediaStreamBitrate::Auto;
    MediaStreamFps fps = MediaStreamFps::Fps30;
    bool any_player = false;

};
/**
 * Plan against the proposed trunk: consolidate branches first, then choose trunk action.
 * SampleFromTrunk action here means "reshape branches only" (trunk encode unchanged).
 */
StreamFanoutPlan build_stream_fanout_plan(
    const SessionClientConnection& requesting_client,
    bool is_seated_player,
    bool session_video_configured,
    const VideoEncodeSettings& current_trunk,
    const VideoEncodeSettings& proposed_trunk,
    SessionVideoCeiling ceilings,
    std::vector<StreamBranchCandidate> candidates,
    bool cutover_in_flight,
    bool within_reconfigure_cooldown);

/** Flatten branch decisions to per-client encode settings for the media fanout. */
std::vector<std::pair<ClientId, VideoEncodeSettings>> branch_layout_client_settings(
    const StreamFanoutPlan& plan);

int stream_fps_rank(MediaStreamFps fps);
MediaStreamFps step_stream_fps_down(MediaStreamFps fps);
MediaStreamFps effective_fps_cap_for(const SessionClientConnection& client);
StreamRequest clamp_stream_request_for_client(
    const SessionClientConnection& client,
    StreamRequest request);

std::optional<StreamAdaptationDecision> adapt_stream_for_heartbeat(
    SessionClientConnection& client,
    const ViewerHeartbeat& heartbeat,
    std::chrono::steady_clock::time_point now,
    std::chrono::steady_clock::time_point started_at,
    std::chrono::seconds startup_grace,
    std::chrono::seconds post_reconfigure_grace,
    std::string_view client_label);



} // namespace archstreamer
