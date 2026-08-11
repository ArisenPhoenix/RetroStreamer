#include "host/client/stream_adaptation.hpp"

#include "host/client/client_stream_policy.hpp"

#include <sstream>
#include <vector>

namespace archstreamer {
namespace {

constexpr std::uint16_t kHighLossPermille = 100;

int stream_bitrate_rank(MediaStreamBitrate bitrate) {
    switch (bitrate) {
    case MediaStreamBitrate::Kbps25000:
        return 7;
    case MediaStreamBitrate::Kbps12000:
        return 6;
    case MediaStreamBitrate::Kbps8000:
        return 5;
    case MediaStreamBitrate::Kbps3500:
        return 4;
    case MediaStreamBitrate::Kbps2500:
        return 3;
    case MediaStreamBitrate::Kbps1500:
        return 2;
    case MediaStreamBitrate::Kbps800:
        return 1;
    case MediaStreamBitrate::Auto:
    default:
        return 0;
    }
}

int stream_size_rank(MediaStreamSize size) {
    switch (size) {
    case MediaStreamSize::P1440:
        return 4;
    case MediaStreamSize::P1080:
        return 3;
    case MediaStreamSize::P720:
        return 2;
    case MediaStreamSize::P540:
        return 1;
    case MediaStreamSize::Auto:
    default:
        return 0;
    }
}

int stream_tier_rank(MediaQualityTier tier) {
    switch (tier) {
    case MediaQualityTier::VeryHigh:
        return 5;
    case MediaQualityTier::High:
        return 4;
    case MediaQualityTier::MediumHigh:
        return 3;
    case MediaQualityTier::Medium:
        return 2;
    case MediaQualityTier::Low:
        return 1;
    case MediaQualityTier::Auto:
    default:
        return 0;
    }
}

int stream_feel_rank_local(MediaStreamFeel feel) {
    switch (feel) {
    case MediaStreamFeel::Smooth:
        return 2;
    case MediaStreamFeel::Balanced:
        return 1;
    case MediaStreamFeel::LowLatency:
    default:
        return 0;
    }
}

int stream_fps_rank_local(MediaStreamFps fps) {
    switch (fps) {
    case MediaStreamFps::Fps60:
        return 5;
    case MediaStreamFps::Fps45:
        return 4;
    case MediaStreamFps::Fps30:
        return 3;
    case MediaStreamFps::Fps24:
        return 2;
    case MediaStreamFps::Fps20:
        return 1;
    case MediaStreamFps::Auto:
    default:
        return 0;
    }
}

bool valid_latency_ms(std::uint16_t value) {
    return value != ViewerHeartbeatLatencyUnknownMs;
}

MediaStreamSize step_stream_size_down(MediaStreamSize size) {
    switch (size) {
    case MediaStreamSize::P1440:
        return MediaStreamSize::P1080;
    case MediaStreamSize::P1080:
        return MediaStreamSize::P720;
    case MediaStreamSize::P720:
        return MediaStreamSize::P540;
    case MediaStreamSize::P540:
    case MediaStreamSize::Auto:
    default:
        return size;
    }
}

MediaStreamBitrate step_stream_bitrate_down(MediaStreamBitrate bitrate) {
    switch (bitrate) {
    case MediaStreamBitrate::Kbps25000:
        return MediaStreamBitrate::Kbps12000;
    case MediaStreamBitrate::Kbps12000:
        return MediaStreamBitrate::Kbps8000;
    case MediaStreamBitrate::Kbps8000:
        return MediaStreamBitrate::Kbps3500;
    case MediaStreamBitrate::Kbps3500:
        return MediaStreamBitrate::Kbps2500;
    case MediaStreamBitrate::Kbps2500:
        return MediaStreamBitrate::Kbps1500;
    case MediaStreamBitrate::Kbps1500:
        return MediaStreamBitrate::Kbps800;
    case MediaStreamBitrate::Kbps800:
    case MediaStreamBitrate::Auto:
    default:
        return bitrate;
    }
}

MediaQualityTier step_stream_tier_down(MediaQualityTier tier) {
    switch (tier) {
    case MediaQualityTier::VeryHigh:
        return MediaQualityTier::High;
    case MediaQualityTier::High:
        return MediaQualityTier::MediumHigh;
    case MediaQualityTier::MediumHigh:
        return MediaQualityTier::Medium;
    case MediaQualityTier::Medium:
        return MediaQualityTier::Low;
    case MediaQualityTier::Low:
    case MediaQualityTier::Auto:
    default:
        return tier;
    }
}

StreamRequest min_stream_request(const StreamRequest& a, const StreamRequest& b) {
    StreamRequest out = a;
    if (stream_size_rank(b.size) > 0 &&
        (stream_size_rank(a.size) == 0 || stream_size_rank(b.size) < stream_size_rank(a.size))) {
        out.size = b.size;
    }
    if (stream_tier_rank(b.tier) > 0 &&
        (stream_tier_rank(a.tier) == 0 || stream_tier_rank(b.tier) < stream_tier_rank(a.tier))) {
        out.tier = b.tier;
    }
    if (stream_feel_rank_local(b.feel) < stream_feel_rank_local(a.feel)) {
        out.feel = b.feel;
    }
    if (stream_bitrate_rank(b.bitrate) > 0 &&
        (stream_bitrate_rank(a.bitrate) == 0 ||
         stream_bitrate_rank(b.bitrate) < stream_bitrate_rank(a.bitrate))) {
        out.bitrate = b.bitrate;
    }
    if (stream_fps_rank_local(b.fps) > 0 &&
        (stream_fps_rank_local(a.fps) == 0 ||
         stream_fps_rank_local(b.fps) < stream_fps_rank_local(a.fps))) {
        out.fps = b.fps;
    }
    return out;
}

bool stream_request_exceeds(const StreamRequest& candidate, const StreamRequest& ceiling) {
    return (stream_size_rank(candidate.size) > 0 &&
            stream_size_rank(ceiling.size) > 0 &&
            stream_size_rank(candidate.size) > stream_size_rank(ceiling.size)) ||
        (stream_tier_rank(candidate.tier) > 0 &&
         stream_tier_rank(ceiling.tier) > 0 &&
         stream_tier_rank(candidate.tier) > stream_tier_rank(ceiling.tier)) ||
        (stream_bitrate_rank(candidate.bitrate) > 0 &&
         stream_bitrate_rank(ceiling.bitrate) > 0 &&
         stream_bitrate_rank(candidate.bitrate) > stream_bitrate_rank(ceiling.bitrate)) ||
        (stream_fps_rank_local(candidate.fps) > 0 &&
         stream_fps_rank_local(ceiling.fps) > 0 &&
         stream_fps_rank_local(candidate.fps) > stream_fps_rank_local(ceiling.fps)) ||
        stream_feel_rank_local(candidate.feel) > stream_feel_rank_local(ceiling.feel);
}

ClientId stream_branch_owner_id(const StreamBranchDecision& decision) {
    if (decision.action == StreamBranchAction::ReuseClient) {
        return decision.reuse_client_id;
    }
    return decision.client_id;
}

} // namespace

const char* stream_pipeline_action_name(StreamPipelineAction action) {
    switch (action) {
    case StreamPipelineAction::SyncToTrunk:
        return "sync-to-trunk";
    case StreamPipelineAction::SampleFromTrunk:
        return "sample-from-trunk";
    case StreamPipelineAction::ReplaceTrunk:
        return "replace-trunk";
    case StreamPipelineAction::HardRestartTrunk:
        return "hard-restart-trunk";
    case StreamPipelineAction::None:
    default:
        return "none";
    }
}

StreamPipelineAction decide_stream_pipeline_action(
    const SessionClientConnection& client,
    bool is_seated_player,
    bool session_video_configured,
    const VideoEncodeSettings& current_trunk,
    const VideoEncodeSettings& proposed_trunk,
    bool cutover_in_flight,
    bool within_reconfigure_cooldown) {
    if (!is_seated_player) {
        return StreamPipelineAction::None;
    }
    if (client.video_cutover.suppressed) {
        return StreamPipelineAction::None;
    }
    if (cutover_in_flight) {
        return StreamPipelineAction::None;
    }
    if (within_reconfigure_cooldown) {
        return StreamPipelineAction::None;
    }
    if (session_video_configured && proposed_trunk == current_trunk) {
        // SampleFromTrunk will branch here once weak-receiver sampling exists.
        return StreamPipelineAction::SyncToTrunk;
    }
    return StreamPipelineAction::ReplaceTrunk;
}

const char* stream_branch_action_name(StreamBranchAction action) {
    switch (action) {
    case StreamBranchAction::SampleFromTrunk:
        return "sample-from-trunk";
    case StreamBranchAction::ReuseClient:
        return "reuse-client";
    case StreamBranchAction::TakeTrunk:
    default:
        return "take-trunk";
    }
}

bool stream_branch_matches_trunk(
    const VideoEncodeSettings& target,
    const VideoEncodeSettings& trunk) {
    return target == trunk;
}

StreamBranchDecision decide_stream_branch_action(
    const StreamBranchCandidate& client,
    const VideoEncodeSettings& trunk,
    const std::vector<StreamBranchDecision>& prior_decisions) {
    StreamBranchDecision decision{};
    decision.client_id = client.client_id;
    if (!client.connected || !client.wants_video) {
        decision.action = StreamBranchAction::TakeTrunk;
        decision.settings = trunk;
        return decision;
    }

    // Caller supplies the receive profile. Anything that matches the trunk (or would
    // require above-trunk encode) rides the trunk; below-trunk profiles sample/reuse.
    VideoEncodeSettings target = client.target_settings;
    const bool above_trunk =
        target.width > trunk.width ||
        target.height > trunk.height ||
        target.bitrate_kbps > trunk.bitrate_kbps ||
        target.framerate > trunk.framerate ||
        target.queue_buffers > trunk.queue_buffers;
    if (above_trunk || stream_branch_matches_trunk(target, trunk)) {
        decision.action = StreamBranchAction::TakeTrunk;
        decision.settings = trunk;
        return decision;
    }

    decision.settings = target;
    for (const auto& prior : prior_decisions) {
        if (prior.settings != target) {
            continue;
        }
        if (prior.action != StreamBranchAction::SampleFromTrunk &&
            prior.action != StreamBranchAction::ReuseClient) {
            continue;
        }
        decision.action = StreamBranchAction::ReuseClient;
        decision.reuse_client_id = stream_branch_owner_id(prior);
        return decision;
    }

    decision.action = StreamBranchAction::SampleFromTrunk;
    return decision;
}

std::vector<StreamBranchDecision> decide_stream_branch_actions(
    const std::vector<StreamBranchCandidate>& clients,
    const VideoEncodeSettings& trunk) {
    std::vector<StreamBranchDecision> decisions;
    decisions.reserve(clients.size());
    for (const auto& client : clients) {
        if (!client.connected || !client.wants_video) {
            continue;
        }
        decisions.push_back(decide_stream_branch_action(client, trunk, decisions));
    }
    return decisions;
}

std::vector<StreamBranchGroup> group_stream_branch_decisions(
    const std::vector<StreamBranchDecision>& decisions,
    const VideoEncodeSettings& trunk) {
    std::vector<StreamBranchGroup> groups;
    StreamBranchGroup* trunk_group = nullptr;

    auto find_sample_group = [&](ClientId owner_id) -> StreamBranchGroup* {
        for (auto& group : groups) {
            if (!group.is_trunk && group.owner_client_id == owner_id) {
                return &group;
            }
        }
        return nullptr;
    };

    for (const auto& decision : decisions) {
        if (decision.action == StreamBranchAction::TakeTrunk ||
            stream_branch_matches_trunk(decision.settings, trunk)) {
            if (trunk_group == nullptr) {
                groups.push_back(StreamBranchGroup{});
                trunk_group = &groups.back();
                trunk_group->is_trunk = true;
                trunk_group->settings = trunk;
                trunk_group->owner_client_id = 0;
            }
            trunk_group->member_client_ids.push_back(decision.client_id);
            continue;
        }

        if (decision.action == StreamBranchAction::SampleFromTrunk) {
            groups.push_back(StreamBranchGroup{
                decision.settings,
                false,
                decision.client_id,
                {decision.client_id},
            });
            continue;
        }

        // ReuseClient — fold into the owner's sample group (or create if missing).
        auto* group = find_sample_group(decision.reuse_client_id);
        if (group == nullptr) {
            groups.push_back(StreamBranchGroup{
                decision.settings,
                false,
                decision.reuse_client_id,
                {decision.client_id},
            });
        } else {
            group->member_client_ids.push_back(decision.client_id);
        }
    }
    return groups;
}

std::vector<StreamClientMigration> migrations_for_stream(const StreamBranchGroup& stream) {
    std::vector<StreamClientMigration> migrations;
    migrations.reserve(stream.member_client_ids.size());
    for (const ClientId client_id : stream.member_client_ids) {
        StreamClientMigration step{};
        step.client_id = client_id;
        step.settings = stream.settings;
        if (stream.is_trunk) {
            step.action = StreamBranchAction::TakeTrunk;
            step.stream_owner_client_id = 0;
        } else if (client_id == stream.owner_client_id) {
            step.action = StreamBranchAction::SampleFromTrunk;
            step.stream_owner_client_id = stream.owner_client_id;
        } else {
            step.action = StreamBranchAction::ReuseClient;
            step.stream_owner_client_id = stream.owner_client_id;
        }
        migrations.push_back(step);
    }
    return migrations;
}

StreamFanoutPlan build_stream_fanout_plan(
    const SessionClientConnection& requesting_client,
    bool is_seated_player,
    bool session_video_configured,
    const VideoEncodeSettings& current_trunk,
    const VideoEncodeSettings& proposed_trunk,
    MediaStreamSize proposed_size,
    MediaQualityTier proposed_tier,
    MediaStreamFeel proposed_feel,
    MediaStreamBitrate proposed_bitrate,
    MediaStreamFps proposed_fps,
    std::vector<StreamBranchCandidate> candidates,
    bool cutover_in_flight,
    bool within_reconfigure_cooldown) {
    StreamFanoutPlan plan;
    plan.current_trunk = current_trunk;
    plan.proposed_trunk = proposed_trunk;
    plan.proposed_size = proposed_size;
    plan.proposed_tier = proposed_tier;
    plan.proposed_feel = proposed_feel;
    plan.proposed_bitrate = proposed_bitrate;
    plan.proposed_fps = proposed_fps;
    plan.trunk_changes =
        !session_video_configured || proposed_trunk != current_trunk;

    // Branch consolidation always targets the destination trunk.
    plan.branches = decide_stream_branch_actions(candidates, proposed_trunk);
    plan.streams = group_stream_branch_decisions(plan.branches, proposed_trunk);

    plan.trunk_action = decide_stream_pipeline_action(
        requesting_client,
        is_seated_player,
        session_video_configured,
        current_trunk,
        proposed_trunk,
        cutover_in_flight,
        within_reconfigure_cooldown);

    if (plan.trunk_action == StreamPipelineAction::None) {
        return plan;
    }

    // Trunk unchanged: SyncToTrunk if everyone rides trunk; SampleFromTrunk if any
    // sample/reuse branch must be (re)shaped.
    if (!plan.trunk_changes &&
        (plan.trunk_action == StreamPipelineAction::SyncToTrunk ||
         plan.trunk_action == StreamPipelineAction::SampleFromTrunk)) {
        bool any_sample = false;
        for (const auto& branch : plan.branches) {
            if (branch.action == StreamBranchAction::SampleFromTrunk ||
                branch.action == StreamBranchAction::ReuseClient) {
                any_sample = true;
                break;
            }
        }
        plan.trunk_action = any_sample
            ? StreamPipelineAction::SampleFromTrunk
            : StreamPipelineAction::SyncToTrunk;
    }

    return plan;
}

std::vector<std::pair<ClientId, VideoEncodeSettings>> branch_layout_client_settings(
    const StreamFanoutPlan& plan) {
    std::vector<std::pair<ClientId, VideoEncodeSettings>> layout;
    layout.reserve(plan.branches.size());
    for (const auto& branch : plan.branches) {
        layout.emplace_back(branch.client_id, branch.settings);
    }
    return layout;
}

int stream_fps_rank(MediaStreamFps fps) {
    switch (fps) {
    case MediaStreamFps::Fps60:
        return 5;
    case MediaStreamFps::Fps45:
        return 4;
    case MediaStreamFps::Fps30:
        return 3;
    case MediaStreamFps::Fps24:
        return 2;
    case MediaStreamFps::Fps20:
        return 1;
    case MediaStreamFps::Auto:
    default:
        return 0;
    }
}

MediaStreamFps step_stream_fps_down(MediaStreamFps fps) {
    switch (fps) {
    case MediaStreamFps::Fps60:
        return MediaStreamFps::Fps45;
    case MediaStreamFps::Fps45:
        return MediaStreamFps::Fps30;
    case MediaStreamFps::Fps30:
        return MediaStreamFps::Fps24;
    case MediaStreamFps::Fps24:
        return MediaStreamFps::Fps20;
    case MediaStreamFps::Fps20:
    case MediaStreamFps::Auto:
    default:
        return fps;
    }
}

MediaStreamFps effective_fps_cap_for(const SessionClientConnection& client) {
    if (client.stream_preferences.adaptive_fps_cap != MediaStreamFps::Auto) {
        return client.stream_preferences.adaptive_fps_cap;
    }
    return media_stream_fps_for_framerate(
        framerate_for_quality_tier(
            select_video_tier(client.stream_preferences.wanted_tier, client.stream_preferences.applied_tier, client.stream_preferences.max_bitrate_kbps)));
}

StreamRequest clamp_stream_request_for_client(
    const SessionClientConnection& client,
    StreamRequest request) {
    const auto policy = client_stream_policy_for(client);
    if (!policy.stream_request_clamp_enabled) {
        return request;
    }

    if (request.size == MediaStreamSize::Auto ||
        media_stream_size_height(request.size) > media_stream_size_height(policy.max_stream_size)) {
        request.size = policy.max_stream_size;
    }
    if (request.tier == MediaQualityTier::MediumHigh ||
        request.tier == MediaQualityTier::High ||
        request.tier == MediaQualityTier::VeryHigh) {
        request.tier = policy.max_auto_bitrate_tier;
    }
    if (stream_bitrate_rank(request.bitrate) > stream_bitrate_rank(policy.max_stream_bitrate)) {
        request.bitrate = policy.max_stream_bitrate;
    }
    if (request.fps == MediaStreamFps::Auto ||
        stream_fps_rank(request.fps) > stream_fps_rank(policy.max_stream_fps)) {
        request.fps = policy.max_stream_fps;
    }
    return request;
}

StreamRequest stream_request_from_applied(const SessionClientConnection& client) {
    return StreamRequest{
        client.stream_preferences.applied_size,
        client.stream_preferences.applied_tier,
        client.stream_preferences.applied_feel,
        client.stream_preferences.applied_bitrate,
        client.stream_preferences.applied_fps,
    };
}

StreamClientHealthMetrics stream_health_metrics_from_client(const SessionClientConnection& client) {
    return StreamClientHealthMetrics{
        client.video_health.last_loss_permille,
        client.video_health.last_frames_decoded_delta,
        client.video_health.decode_queue_p95_ms,
        client.video_health.decode_queue_max_ms,
        client.video_health.au_queue_p95_ms,
    };
}

StreamClientHealthMetrics stream_health_metrics_from_heartbeat(const ViewerHeartbeat& heartbeat) {
    return StreamClientHealthMetrics{
        heartbeat.loss_permille,
        heartbeat.frames_decoded_delta,
        heartbeat.decode_queue_p95_ms,
        heartbeat.decode_queue_max_ms,
        heartbeat.au_queue_p95_ms,
    };
}

StreamRequestResolution resolve_stream_request_for_client(
    SessionClientConnection& client,
    const StreamRequest& current,
    const StreamRequest& requested,
    const StreamClientHealthMetrics& health) {
    StreamRequestResolution resolution;
    const auto policy = client_stream_policy_for(client);
    resolution.request = clamp_stream_request_for_client(client, requested);

    if (resolution.request.tier != MediaQualityTier::Auto) {
        resolution.request.tier = select_video_tier(
            resolution.request.tier,
            current.tier == MediaQualityTier::Auto ? client.stream_preferences.applied_tier : current.tier,
            client.stream_preferences.max_bitrate_kbps);
    }

    if (client.stream_preferences.adaptive_fps_cap != MediaStreamFps::Auto) {
        if (resolution.request.fps == MediaStreamFps::Auto ||
            stream_fps_rank(resolution.request.fps) > stream_fps_rank(client.stream_preferences.adaptive_fps_cap)) {
            resolution.request.fps = client.stream_preferences.adaptive_fps_cap;
            resolution.limited_by_metrics = true;
            resolution.reason = "adaptive fps cap";
        }
    }

    const bool hard_loss = health.loss_permille >= kHighLossPermille;
    const bool decode_pressure =
        valid_latency_ms(health.decode_queue_p95_ms) &&
        health.decode_queue_p95_ms >= policy.decode_pressure_step_down_p95_ms &&
        health.frames_decoded_delta > 0 &&
        !hard_loss;

    if (hard_loss || decode_pressure) {
        if (stream_request_exceeds(resolution.request, current)) {
            resolution.request = min_stream_request(resolution.request, current);
            resolution.limited_by_metrics = true;
            resolution.reason = hard_loss ? "loss: hold current" : "decode pressure: hold current";
        }
    }

    if (decode_pressure) {
        if (client.video_health.decode_pressure_streak < 255) {
            ++client.video_health.decode_pressure_streak;
        }
        if (client.video_health.decode_pressure_streak >= policy.decode_pressure_bad_heartbeats) {
            client.video_health.decode_pressure_streak = 0;
            MediaStreamFps fps = resolution.request.fps;
            if (fps == MediaStreamFps::Auto) {
                fps = current.fps == MediaStreamFps::Auto ? MediaStreamFps::Fps30 : current.fps;
            }
            const auto next_fps = step_stream_fps_down(fps);
            if (next_fps != fps) {
                resolution.request.fps = next_fps;
                client.stream_preferences.adaptive_fps_cap = next_fps;
                resolution.limited_by_metrics = true;
                resolution.reason = "decode pressure: step fps";
            } else {
                MediaStreamBitrate bitrate = resolution.request.bitrate;
                if (bitrate == MediaStreamBitrate::Auto) {
                    bitrate = current.bitrate == MediaStreamBitrate::Auto
                        ? MediaStreamBitrate::Kbps3500
                        : current.bitrate;
                }
                const auto next_bitrate = step_stream_bitrate_down(bitrate);
                if (next_bitrate != bitrate) {
                    resolution.request.bitrate = next_bitrate;
                    resolution.limited_by_metrics = true;
                    resolution.reason = "decode pressure: step bitrate";
                } else {
                    MediaStreamSize size = resolution.request.size;
                    if (size == MediaStreamSize::Auto) {
                        size = current.size == MediaStreamSize::Auto
                            ? MediaStreamSize::P720
                            : current.size;
                    }
                    const auto next_size = step_stream_size_down(size);
                    if (next_size != size) {
                        resolution.request.size = next_size;
                        resolution.limited_by_metrics = true;
                        resolution.reason = "decode pressure: step size";
                    } else {
                        MediaQualityTier tier = resolution.request.tier;
                        if (tier == MediaQualityTier::Auto) {
                            tier = current.tier == MediaQualityTier::Auto
                                ? MediaQualityTier::Medium
                                : current.tier;
                        }
                        const auto next_tier = step_stream_tier_down(tier);
                        if (next_tier != tier) {
                            resolution.request.tier = next_tier;
                            resolution.limited_by_metrics = true;
                            resolution.reason = "decode pressure: step tier";
                        }
                    }
                }
            }
        }
    } else {
        client.video_health.decode_pressure_streak = 0;
    }

    return resolution;
}

std::optional<StreamAdaptationDecision> adapt_stream_for_heartbeat(
    SessionClientConnection& client,
    const ViewerHeartbeat& heartbeat,
    std::chrono::steady_clock::time_point now,
    std::chrono::steady_clock::time_point started_at,
    std::chrono::seconds startup_grace,
    std::chrono::seconds post_reconfigure_grace,
    std::string_view client_label) {
    if (now - started_at < startup_grace) {
        return std::nullopt;
    }
    if (client.video_health.last_video_reconfigure.time_since_epoch().count() != 0 &&
        now - client.video_health.last_video_reconfigure < post_reconfigure_grace) {
        return std::nullopt;
    }
    const bool hard_loss = heartbeat.loss_permille >= kHighLossPermille;
    const auto policy = client_stream_policy_for(client);
    const bool tv_decode_pressure =
        policy.is_tv &&
        valid_latency_ms(client.video_health.decode_queue_p95_ms) &&
        client.video_health.decode_queue_p95_ms >= policy.decode_pressure_step_down_p95_ms &&
        heartbeat.frames_decoded_delta > 0 &&
        !hard_loss;
    if (!tv_decode_pressure) {
        return std::nullopt;
    }

    // Streak / step-down now live in resolve_stream_request_for_client; keep this
    // hook as a no-op notifier so older call sites still compile.
    return StreamAdaptationDecision{};
}

} // namespace archstreamer
