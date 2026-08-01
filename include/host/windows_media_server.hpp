#pragma once

#include "common/platform/default_platform.hpp"
#include "host/media_capture.hpp"
#include "host/media_server.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace archstreamer {

struct WindowsMediaCaptureConfig {
    bool video = false;
    bool audio = false;
    std::string video_resolution = "1920x1080";
    bool verbose = false;
    int nvenc_cuda_device_id = -1;
};

class WindowsMediaServer final : public MediaServer {
public:
    explicit WindowsMediaServer(WindowsMediaCaptureConfig capture);

    void start(
        const HostMediaPlanConfig& plan,
        const std::vector<HostMediaDestination>& destinations,
        std::vector<MediaClientStream>& streams) override;
    MediaEndpoint add_client(
        ClientId client_id,
        const std::string& destination_host,
        std::size_t media_index,
        bool wants_video,
        bool wants_audio) override;
    void remove_client(ClientId client_id) override;
    std::optional<std::string> begin_video_tier_cutover(
        ClientId client_id,
        const VideoEncodeSettings& settings) override;
    bool complete_video_tier_cutover(
        ClientId client_id,
        std::string_view staging_video_uri) override;
    void abort_video_tier_cutover(ClientId client_id) override;
    bool video_cutover_in_flight(ClientId client_id) const override;
    void stop() override;

private:
    void restart_video();
    void restart_audio();

    WindowsMediaCaptureConfig capture_;
    HostMediaPlanConfig plan_{};
    std::vector<HostMediaDestination> destinations_;
    ChildProcess video_process_;
    ChildProcess audio_process_;
    bool video_running_ = false;
    bool audio_running_ = false;
};

std::unique_ptr<MediaServer> make_windows_media_server(const WindowsMediaCaptureConfig& capture);

} // namespace archstreamer
