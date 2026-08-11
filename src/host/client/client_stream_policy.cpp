#include "host/client/client_stream_policy.hpp"

#include <algorithm>

namespace archstreamer {
namespace {

constexpr std::uint8_t kDecodePressureBadHeartbeats = 6;
constexpr std::uint16_t kTvDecodePressureStepDownP95Ms = 200;
constexpr std::uint16_t kDecodePressureStepDownP95Ms = 160;
constexpr bool kEnableTvStreamRequestClamp = false;
constexpr MediaStreamSize kTvMaxStreamSize = MediaStreamSize::P540;
constexpr MediaQualityTier kTvMaxAutoBitrateTier = MediaQualityTier::Medium;
constexpr MediaStreamBitrate kTvMaxStreamBitrate = MediaStreamBitrate::Kbps3500;
constexpr MediaStreamFps kTvMaxStreamFps = MediaStreamFps::Fps20;

} // namespace

bool client_is_tv(const SessionClientConnection& client) {
    return client.hello.device.device_class == ClientDeviceClass::Tv;
}

bool client_is_seated_player(const SessionClientConnection& client) {
    return client.hello.requested_players > 0;
}

bool session_has_seated_tv_player(const SessionPlan& plan) {
    return std::any_of(
        plan.clients.begin(),
        plan.clients.end(),
        [](const SessionClientConnection& client) {
            return client_is_seated_player(client) && client_is_tv(client);
        });
}

bool session_prefers_switch_handheld_mode(const SessionPlan& plan) {
    return session_has_seated_tv_player(plan);
}

ClientStreamPolicy client_stream_policy_for(const SessionClientConnection& client) {
    ClientStreamPolicy policy{};
    policy.is_tv = client_is_tv(client);
    policy.is_seated_player = client_is_seated_player(client);
    policy.align_encode_to_macroblocks = policy.is_tv;
    policy.stream_request_clamp_enabled =
        policy.is_tv && kEnableTvStreamRequestClamp;
    policy.decode_pressure_step_down_p95_ms = policy.is_tv
        ? kTvDecodePressureStepDownP95Ms
        : kDecodePressureStepDownP95Ms;
    policy.decode_pressure_bad_heartbeats = kDecodePressureBadHeartbeats;
    policy.prefer_switch_handheld_mode = policy.is_tv && policy.is_seated_player;
    policy.max_stream_size = kTvMaxStreamSize;
    policy.max_auto_bitrate_tier = kTvMaxAutoBitrateTier;
    policy.max_stream_bitrate = kTvMaxStreamBitrate;
    policy.max_stream_fps = kTvMaxStreamFps;
    return policy;
}

InitialSessionVideoPolicy initial_session_video_policy_for(const SessionPlan& plan) {
    if (session_has_seated_tv_player(plan)) {
        return InitialSessionVideoPolicy{
            MediaStreamSize::P540,
            MediaQualityTier::Low,
            MediaStreamBitrate::Kbps3500,
            MediaStreamFps::Fps20,
            true,
            "seated-tv-player",
        };
    }
    return InitialSessionVideoPolicy{};
}

VideoEncodeSettings align_encode_to_h264_macroblocks(VideoEncodeSettings settings) {
    // Chromecast/Amlogic decoders can expose padded rows after recovery if the
    // encoded height is not macroblock-aligned, e.g. 540p. Encode those rows
    // explicitly for TV sessions instead of relying on crop metadata.
    settings.width = static_cast<std::uint16_t>((settings.width + 15u) & ~15u);
    settings.height = static_cast<std::uint16_t>((settings.height + 15u) & ~15u);
    return settings;
}

VideoEncodeSettings apply_client_stream_policy(
    VideoEncodeSettings settings,
    const ClientStreamPolicy& policy) {
    if (policy.align_encode_to_macroblocks) {
        return align_encode_to_h264_macroblocks(settings);
    }
    return settings;
}

} // namespace archstreamer
