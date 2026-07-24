#pragma once

#include "common/protocol.hpp"
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

AudioCaptureBackend choose_audio_capture_backend(AudioCaptureBackend requested);
std::string default_audio_monitor_source();
// Dedicated null sink for RetroArch while streaming so the host speakers stay silent
// unless "Watch stream locally" (or a remote) plays the RTP feed.
std::string ensure_streaming_audio_sink();
std::string streaming_audio_monitor_source();
// Keep Viewer RetroArch on the streaming null sink (defeats PipeWire stream-restore leaks).
void park_streaming_game_audio();
// If the session default was left on the streaming null sink, point it back at a real device.
void restore_default_sink_after_streaming();

// One encode shared by Watch-local + all remotes (multiudpsink).
// Source is either an X11 display (ximagesrc) or a PipeWire node (pipewiresrc).
class GStreamerVideoFanout {
public:
    ~GStreamerVideoFanout();

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
    bool reconfigure_client(ClientId client_id, const VideoEncodeSettings& settings);
    void stop();
    void stop_client(ClientId client_id);

private:
    struct Destination {
        ClientId client_id = 0;
        std::string host;
        std::uint16_t port = 0;
        VideoEncodeSettings settings;
    };

    void restart_pipeline();

    enum class SourceKind { X11, PipeWire };
    SourceKind source_kind_ = SourceKind::X11;
    std::string display_;
    std::string pipewire_node_;
    VideoEncodeSettings shared_settings_;
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

struct GStreamerMediaCaptureConfig {
    bool video = false;
    bool audio = false;
    std::string virtual_display = ":99";
    std::string video_resolution = "1280x720";
    VirtualDisplayBackend display_backend = VirtualDisplayBackend::None;
    AudioCaptureBackend audio_backend = AudioCaptureBackend::Pulse;
    std::string audio_source;
    bool verbose = false;
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
