#pragma once

#include "common/addresses.hpp"
#include "common/media.hpp"
#include "common/protocol.hpp"
#include "host/video/media_config.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace archstreamer {

/**
 * Runtime send plan MediaServer.start() consumes. Host builds this from
 * MediaConfig (and may override ports/encode after cadence grants).
 */
struct MediaPlanConfig {
    bool video = false;
    bool audio = false;
    std::string destination_host = "127.0.0.1";
    bool destination_host_explicit = false;
    std::uint16_t video_port = 5004;
    std::uint16_t audio_port = 6004;
    bool include_host_loopback = false;
    VideoEncodeSettings initial_video_settings =
        video_encode_settings(MediaStreamSize::P720, MediaQualityTier::Medium);
};

struct MediaDestination {
    ClientId client_id = 0;
    std::string host;
};

inline MediaPlanConfig make_media_plan_config(const MediaConfig& media) {
    MediaPlanConfig plan;
    plan.video = media.video.enabled;
    plan.audio = media.audio.enabled;
    plan.destination_host = media.video.destination;
    plan.destination_host_explicit = media.video.destination_explicit;
    plan.video_port = media.video.port;
    plan.audio_port = media.audio.port;
    plan.include_host_loopback = media.host_local_watch;
    plan.initial_video_settings =
        video_encode_settings(MediaStreamSize::P720, MediaQualityTier::Medium);
    return plan;
}

inline MediaStreamRequest video_request_for_destination(
    const MediaPlanConfig& config,
    const MediaDestination& destination,
    std::size_t media_index) {
    return MediaStreamRequest{
        destination.client_id,
        destination.host,
        static_cast<std::uint16_t>(config.video_port + media_index),
    };
}

inline MediaStreamRequest audio_request_for_destination(
    const MediaPlanConfig& config,
    const MediaDestination& destination,
    std::size_t media_index) {
    return MediaStreamRequest{
        destination.client_id,
        destination.host,
        static_cast<std::uint16_t>(config.audio_port + media_index),
    };
}

inline std::vector<MediaStreamRequest> video_requests_from_media_destinations(
    const MediaPlanConfig& config,
    const std::vector<MediaDestination>& destinations) {
    std::vector<MediaStreamRequest> requests;
    requests.reserve(destinations.size());
    for (std::size_t index = 0; index < destinations.size(); ++index) {
        requests.push_back(video_request_for_destination(config, destinations[index], index));
    }
    return requests;
}

inline std::vector<MediaStreamRequest> audio_requests_from_media_destinations(
    const MediaPlanConfig& config,
    const std::vector<MediaDestination>& destinations) {
    std::vector<MediaStreamRequest> requests;
    requests.reserve(destinations.size());
    for (std::size_t index = 0; index < destinations.size(); ++index) {
        requests.push_back(audio_request_for_destination(config, destinations[index], index));
    }
    return requests;
}

inline std::vector<MediaClientStream> media_streams_for_dry_run(
    const MediaPlanConfig& config,
    const std::vector<MediaDestination>& destinations) {
    std::vector<MediaClientStream> streams;
    streams.reserve(destinations.size());
    for (std::size_t index = 0; index < destinations.size(); ++index) {
        const auto& destination = destinations[index];
        MediaEndpoint endpoint;
        if (config.video) {
            endpoint.video_uri = rtp_h264_uri(
                destination.host,
                static_cast<std::uint16_t>(config.video_port + index));
        }
        if (config.audio) {
            endpoint.audio_uri = rtp_opus_uri(
                destination.host,
                static_cast<std::uint16_t>(config.audio_port + index));
        }
        streams.push_back(MediaClientStream{
            destination.client_id,
            destination.host,
            endpoint,
        });
    }
    return streams;
}

class MediaServer {
public:
    virtual ~MediaServer() = default;

    virtual void start(
        const MediaPlanConfig& plan,
        const std::vector<MediaDestination>& destinations,
        std::vector<MediaClientStream>& streams) = 0;
    virtual MediaEndpoint add_client(
        ClientId client_id,
        const std::string& destination_host,
        std::size_t media_index,
        bool wants_video,
        bool wants_audio) = 0;
    virtual void remove_client(ClientId client_id) = 0;

    /**
     * Restart the single shared encode at [settings] for every video destination
     * (hard restart on the same client ports). Returns false if video fanout is
     * unavailable / empty.
     */
    virtual bool reconfigure_shared_video(const VideoEncodeSettings& settings) = 0;
    /**
     * Apply per-client encode settings and rebuild the capture ladder.
     * Clients with matching settings share one encode branch (trunk or sample).
     */
    virtual bool apply_video_branch_layout(
        const VideoEncodeSettings& trunk,
        const std::vector<std::pair<ClientId, VideoEncodeSettings>>& per_client) = 0;
    /** Restart the shared audio fanout without changing client ports. */
    virtual bool restart_shared_audio() = 0;

    /**
     * Legacy dual-stream cutover (unused for quality changes — session uses
     * reconfigure_shared_video). Kept so older clients' MediaVideoReady is harmless.
     */
    virtual std::optional<std::string> begin_video_tier_cutover(
        ClientId client_id,
        const VideoEncodeSettings& settings) = 0;
    virtual bool complete_video_tier_cutover(
        ClientId client_id,
        std::string_view staging_video_uri) = 0;
    virtual void abort_video_tier_cutover(ClientId client_id) = 0;
    virtual bool video_cutover_in_flight(ClientId client_id) const = 0;
    /** Current RTP H.264 URI the client should receive on (base or promoted port). */
    virtual std::optional<std::string> current_video_uri(ClientId client_id) const = 0;

    virtual void stop() = 0;
};

} // namespace archstreamer
