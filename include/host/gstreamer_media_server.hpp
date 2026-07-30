#pragma once

#include "common/protocol.hpp"
#include "common/platform/default_platform.hpp"
#include "host/media_capture.hpp"
#include "host/media_server.hpp"
#include "host/virtual_display.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace archstreamer {

// Encode ladder: one capture tee into quality-tier branches; each client is
// assigned to one tier's multiudpsink (same receive port, different encode).
class GStreamerVideoFanout {
public:
    ~GStreamerVideoFanout();

    void set_nvenc_cuda_device_id(int nvidia_index) { nvenc_cuda_device_id_ = nvidia_index; }

    std::vector<MediaClientStream> start(
        const std::string& display,
        const std::vector<MediaStreamRequest>& destinations);
    std::vector<MediaClientStream> start_pipewire(
        const std::string& pipewire_node,
        const std::vector<MediaStreamRequest>& destinations);
    MediaClientStream add(
        const std::string& display,
        const MediaStreamRequest& destination,
        const VideoEncodeSettings& settings = {});
    // Move client onto the ladder tier matching settings (selector); rebuild sinks.
    bool reconfigure_client(ClientId client_id, const VideoEncodeSettings& settings);
    void stop();
    void stop_client(ClientId client_id);

private:
    struct Destination {
        ClientId client_id = 0;
        std::string host;
        std::uint16_t port = 0;
        MediaQualityTier tier = MediaQualityTier::Medium;
    };

    void restart_pipeline();

    enum class SourceKind { X11, PipeWire };
    SourceKind source_kind_ = SourceKind::X11;
    std::string display_;
    std::string pipewire_node_;
    // nvidia-smi index for nvenc via CUDA_VISIBLE_DEVICES; -1 = leave unset.
    int nvenc_cuda_device_id_ = -1;
    std::vector<Destination> destinations_;
    ChildProcess process_;
};

// One pulsesrc/opus encode shared by Watch-local + all remotes (multiudpsink).
class GStreamerAudioFanout {
public:
    ~GStreamerAudioFanout();

    std::vector<MediaClientStream> start(
        AudioCaptureBackend backend,
        const std::string& source,
        const std::vector<MediaStreamRequest>& destinations);
    MediaClientStream add(
        AudioCaptureBackend backend,
        const std::string& source,
        const MediaStreamRequest& destination);
    void stop();
    void stop_client(ClientId client_id);
    /** Rebuild the shared Opus encode (pair with video ladder restarts for A/V realign). */
    void restart();

private:
    struct Destination {
        ClientId client_id = 0;
        std::string host;
        std::uint16_t port = 0;
    };

    void restart_pipeline();

    AudioCaptureBackend backend_ = AudioCaptureBackend::Pulse;
    std::string source_;
    std::vector<Destination> destinations_;
    ChildProcess process_;
};

class GStreamerMediaServer final : public MediaServer {
public:
    explicit GStreamerMediaServer(GStreamerMediaCaptureConfig capture);

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
    bool reconfigure_client_video(ClientId client_id, const VideoEncodeSettings& settings) override;
    void stop() override;

    // Gamescope: video fanout is deferred until the PipeWire node appears after launch.
    [[nodiscard]] bool video_deferred() const;
    void start_pipewire_video(
        const std::string& pipewire_node,
        std::vector<MediaClientStream>& streams);

private:
    GStreamerMediaCaptureConfig capture_;
    HostMediaPlanConfig plan_;
    std::vector<HostMediaDestination> destinations_;
    bool defer_pipewire_video_ = false;
    std::unique_ptr<VirtualDisplay> virtual_display_;
    std::optional<GStreamerVideoFanout> video_fanout_;
    std::optional<GStreamerAudioFanout> audio_fanout_;
};

std::unique_ptr<MediaServer> make_gstreamer_media_server(const GStreamerMediaCaptureConfig& capture);

} // namespace archstreamer
