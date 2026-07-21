#include "host/host_app_config.hpp"

namespace archstreamer {

HostMediaPlanConfig media_plan_config_for(const HostAppConfig& config) {
    return HostMediaPlanConfig{
        config.video,
        config.audio,
        config.video_destination,
        config.video_destination_explicit,
        config.video_port,
        config.audio_port,
    };
}

} // namespace archstreamer
