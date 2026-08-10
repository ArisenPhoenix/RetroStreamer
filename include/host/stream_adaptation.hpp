#pragma once

#include "common/protocol.hpp"
#include "host/session_lobby.hpp"

#include <chrono>
#include <optional>
#include <string>

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
