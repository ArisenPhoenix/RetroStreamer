#pragma once

#include "common/platform/default_platform.hpp"
#include "common/protocol.hpp"
#include "host/media_capture.hpp"
#include "host/media_server.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace archstreamer {

bool command_available(const char* command);
VirtualDisplayBackend choose_virtual_display_backend(VirtualDisplayBackend requested);
AudioCaptureBackend choose_audio_capture_backend(AudioCaptureBackend requested);
std::string default_audio_monitor_source();
// Dedicated null sink for RetroArch while streaming so the host speakers stay silent
// unless "Watch stream locally" (or a remote) plays the RTP feed.
std::string ensure_streaming_audio_sink();
std::string streaming_audio_monitor_source();

class VirtualDisplayProcess {
public:
    ~VirtualDisplayProcess();

    void start(VirtualDisplayBackend backend, const std::string& display, const std::string& resolution);
    void stop();

private:
    VirtualDisplayBackend backend_ = VirtualDisplayBackend::None;
    ChildProcess process_;
};

class GStreamerVideoSender {
public:
    MediaEndpoint endpoint(std::string host, std::uint16_t port) const;
    void start(
        const std::string& display,
        const std::string& destination_host,
        std::uint16_t port,
        const VideoEncodeSettings& settings = {});
    void stop();

private:
    ChildProcess process_;
};

class GStreamerVideoFanout {
public:
    ~GStreamerVideoFanout();

    std::vector<MediaClientStream> start(
        const std::string& display,
        const std::vector<MediaStreamRequest>& destinations);
    MediaClientStream add(
        const std::string& display,
        const MediaStreamRequest& destination,
        const VideoEncodeSettings& settings = {});
    bool reconfigure_client(ClientId client_id, const VideoEncodeSettings& settings);
    void stop();
    void stop_client(ClientId client_id);

private:
    struct ClientVideoSender {
        GStreamerVideoSender sender;
        std::string destination_host;
        std::uint16_t port = 0;
        VideoEncodeSettings settings;
    };

    std::string display_;
    std::map<ClientId, ClientVideoSender> senders_;
};

class GStreamerAudioSender {
public:
    MediaEndpoint endpoint(std::string host, std::uint16_t port) const;
    void start(
        AudioCaptureBackend backend,
        const std::string& source,
        const std::string& destination_host,
        std::uint16_t port);
    void stop();

private:
    ChildProcess process_;
};

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

private:
    std::map<ClientId, GStreamerAudioSender> senders_;
};

struct GStreamerMediaCaptureConfig {
    bool video = false;
    bool audio = false;
    std::string virtual_display = ":99";
    std::string video_resolution = "1280x720";
    VirtualDisplayBackend display_backend = VirtualDisplayBackend::None;
    AudioCaptureBackend audio_backend = AudioCaptureBackend::Pulse;
    std::string audio_source;
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

private:
    GStreamerMediaCaptureConfig capture_;
    HostMediaPlanConfig plan_;
    std::optional<VirtualDisplayProcess> virtual_display_;
    std::optional<GStreamerVideoFanout> video_fanout_;
    std::optional<GStreamerAudioFanout> audio_fanout_;
};

std::unique_ptr<MediaServer> make_gstreamer_media_server(const GStreamerMediaCaptureConfig& capture);

} // namespace archstreamer
