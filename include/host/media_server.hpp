#pragma once

#include "common/media.hpp"
#include "common/protocol.hpp"
#include "host/host_launch_planner.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace archstreamer {

class MediaServer {
public:
    virtual ~MediaServer() = default;

    virtual void start(
        const HostMediaPlanConfig& plan,
        const std::vector<HostMediaDestination>& destinations,
        std::vector<MediaClientStream>& streams) = 0;
    virtual MediaEndpoint add_client(
        ClientId client_id,
        const std::string& destination_host,
        std::size_t media_index,
        bool wants_video,
        bool wants_audio) = 0;
    virtual void remove_client(ClientId client_id) = 0;

    /**
     * Warm a dedicated encode for the new tier on a staging port without stopping
     * the client's current RTP. Returns the staging video URI, or nullopt if the
     * request is a no-op / busy / unsupported.
     */
    virtual std::optional<std::string> begin_video_tier_cutover(
        ClientId client_id,
        const VideoEncodeSettings& settings) = 0;
    /** Promote staging to active and tear down the previous encode path. */
    virtual bool complete_video_tier_cutover(
        ClientId client_id,
        std::string_view staging_video_uri) = 0;
    virtual void abort_video_tier_cutover(ClientId client_id) = 0;
    virtual bool video_cutover_in_flight(ClientId client_id) const = 0;

    virtual void stop() = 0;
};

} // namespace archstreamer
