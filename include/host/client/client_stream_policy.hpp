#pragma once

#include "common/protocol.hpp"
#include "host/session/lobby.hpp"

#include <cstdint>

namespace archstreamer {

struct ClientStreamPolicy {
    bool is_tv = false;
    bool is_seated_player = false;
    bool align_encode_to_macroblocks = false;
    bool stream_request_clamp_enabled = false;
    MediaStreamSize max_stream_size = MediaStreamSize::P540;
    MediaQualityTier max_auto_bitrate_tier = MediaQualityTier::Medium;
    MediaStreamBitrate max_stream_bitrate = MediaStreamBitrate::Kbps3500;
    MediaStreamFps max_stream_fps = MediaStreamFps::Fps20;
    std::uint16_t decode_pressure_step_down_p95_ms = 160;
    std::uint8_t decode_pressure_bad_heartbeats = 6;
    bool prefer_switch_handheld_mode = false;
};

struct InitialSessionVideoPolicy {
    MediaStreamSize size = MediaStreamSize::P720;
    MediaQualityTier tier = MediaQualityTier::Medium;
    MediaStreamBitrate bitrate = MediaStreamBitrate::Auto;
    MediaStreamFps fps = MediaStreamFps::Fps30;
    bool align_encode_to_macroblocks = false;
    const char* reason = "default";
};

bool client_is_tv(const SessionClientConnection& client);
bool client_is_seated_player(const SessionClientConnection& client);
bool session_has_seated_tv_player(const SessionPlan& plan);
bool session_prefers_switch_handheld_mode(const SessionPlan& plan);

ClientStreamPolicy client_stream_policy_for(const SessionClientConnection& client);
InitialSessionVideoPolicy initial_session_video_policy_for(const SessionPlan& plan);
VideoEncodeSettings align_encode_to_h264_macroblocks(VideoEncodeSettings settings);
VideoEncodeSettings apply_client_stream_policy(
    VideoEncodeSettings settings,
    const ClientStreamPolicy& policy);

} // namespace archstreamer
