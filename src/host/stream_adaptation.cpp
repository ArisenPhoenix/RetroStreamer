#include "host/stream_adaptation.hpp"

#include <sstream>

namespace archstreamer {
namespace {

constexpr std::uint8_t kTvDecodePressureBadHeartbeats = 3;
constexpr std::uint16_t kHighLossPermille = 100;
constexpr std::uint16_t kTvDecodePressureStepDownP95Ms = 100;
constexpr bool kEnableTvStreamRequestClamp = false;
constexpr MediaStreamSize kTvMaxStreamSize = MediaStreamSize::P540;
constexpr MediaQualityTier kTvMaxAutoBitrateTier = MediaQualityTier::Medium;
constexpr MediaStreamBitrate kTvMaxStreamBitrate = MediaStreamBitrate::Kbps3500;
constexpr MediaStreamFps kTvMaxStreamFps = MediaStreamFps::Fps20;

bool client_is_tv(const SessionClientConnection& client) {
    return client.hello.device.device_class == ClientDeviceClass::Tv;
}

int stream_bitrate_rank(MediaStreamBitrate bitrate) {
    switch (bitrate) {
    case MediaStreamBitrate::Kbps25000:
        return 5;
    case MediaStreamBitrate::Kbps12000:
        return 4;
    case MediaStreamBitrate::Kbps8000:
        return 3;
    case MediaStreamBitrate::Kbps3500:
        return 2;
    case MediaStreamBitrate::Kbps800:
        return 1;
    case MediaStreamBitrate::Auto:
    default:
        return 0;
    }
}

bool valid_latency_ms(std::uint16_t value) {
    return value != ViewerHeartbeatLatencyUnknownMs;
}

} // namespace

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
    if (client.adaptive_fps_cap != MediaStreamFps::Auto) {
        return client.adaptive_fps_cap;
    }
    return media_stream_fps_for_framerate(
        framerate_for_quality_tier(
            select_video_tier(client.wanted_tier, client.applied_tier, client.max_bitrate_kbps)));
}

StreamRequest clamp_stream_request_for_client(
    const SessionClientConnection& client,
    StreamRequest request) {
    if (!kEnableTvStreamRequestClamp || !client_is_tv(client)) {
        return request;
    }

    if (request.size == MediaStreamSize::Auto ||
        media_stream_size_height(request.size) > media_stream_size_height(kTvMaxStreamSize)) {
        request.size = kTvMaxStreamSize;
    }
    if (request.tier == MediaQualityTier::MediumHigh ||
        request.tier == MediaQualityTier::High ||
        request.tier == MediaQualityTier::VeryHigh) {
        request.tier = kTvMaxAutoBitrateTier;
    }
    if (stream_bitrate_rank(request.bitrate) > stream_bitrate_rank(kTvMaxStreamBitrate)) {
        request.bitrate = kTvMaxStreamBitrate;
    }
    if (request.fps == MediaStreamFps::Auto ||
        stream_fps_rank(request.fps) > stream_fps_rank(kTvMaxStreamFps)) {
        request.fps = kTvMaxStreamFps;
    }
    return request;
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
    if (client.last_video_reconfigure.time_since_epoch().count() != 0 &&
        now - client.last_video_reconfigure < post_reconfigure_grace) {
        return std::nullopt;
    }
    const bool hard_loss = heartbeat.loss_permille >= kHighLossPermille;
    const bool tv_decode_pressure =
        client_is_tv(client) &&
        valid_latency_ms(client.decode_queue_p95_ms) &&
        client.decode_queue_p95_ms >= kTvDecodePressureStepDownP95Ms &&
        heartbeat.frames_decoded_delta > 0 &&
        !hard_loss;
    if (!tv_decode_pressure) {
        return std::nullopt;
    }

    ++client.bad_health_streak;
    client.good_health_streak = 0;
    if (client.bad_health_streak < kTvDecodePressureBadHeartbeats) {
        return StreamAdaptationDecision{};
    }

    const auto current_cap = effective_fps_cap_for(client);
    const auto next_cap = step_stream_fps_down(current_cap);
    client.bad_health_streak = 0;
    if (next_cap == current_cap) {
        return StreamAdaptationDecision{};
    }

    client.adaptive_fps_cap = next_cap;

    std::ostringstream log;
    log
        << "TV adaptive FPS cap -> " << media_stream_fps_name(next_cap)
        << " for " << client_label
        << " decode_queue_p95=" << client.decode_queue_p95_ms
        << "ms decode_queue_max=" << client.decode_queue_max_ms
        << "ms au_queue_p95=" << client.au_queue_p95_ms
        << "ms loss=" << heartbeat.loss_permille << "‰";

    return StreamAdaptationDecision{
        client.applied_size,
        client.applied_tier,
        client.applied_feel,
        client.applied_bitrate,
        next_cap,
        "host TV FPS cap",
        log.str(),
    };
}

} // namespace archstreamer
