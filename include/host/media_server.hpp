#pragma once

#include "common/media.hpp"
#include "common/protocol.hpp"
#include "host/host_launch_planner.hpp"

#include <cstddef>
#include <string>
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
    // Restart that client's video gst-launch with new encode settings. Returns false if no video sender.
    virtual bool reconfigure_client_video(ClientId client_id, const VideoEncodeSettings& settings) = 0;
    virtual void stop() = 0;
};

} // namespace archstreamer
