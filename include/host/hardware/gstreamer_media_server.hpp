#pragma once

#include "common/protocol.hpp"
#include "common/platform/default_platform.hpp"
#include "host/hardware/media_capture.hpp"
#include "host/hardware/media_server.hpp"
#include "host/virtual/virtual_display.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace archstreamer {

class SharedVideoPipelineRunner {
public:
    virtual ~SharedVideoPipelineRunner() = default;
    virtual void stop() = 0;
    [[nodiscard]] virtual bool running() const = 0;
};

// Encode ladder: one capture → one shared H.264 encode → multiudpsink fanout.
// Mid-session quality changes may stage a dedicated encode for the player before
// promoting it, leaving the old shared encode alive for viewers during warm-up.
class GStreamerVideoFanout {
public:
    ~GStreamerVideoFanout();

    void set_nvenc_cuda_device_id(int nvidia_index) { nvenc_cuda_device_id_ = nvidia_index; }

    std::vector<MediaClientStream> start(
        const std::string& display,
        const std::vector<MediaStreamRequest>& destinations,
        const VideoEncodeSettings& initial_settings = {});
    std::vector<MediaClientStream> start_pipewire(
        const std::string& pipewire_node,
        const std::vector<MediaStreamRequest>& destinations,
        const VideoEncodeSettings& initial_settings = {});
    MediaClientStream add(
        const std::string& display,
        const MediaStreamRequest& destination,
        const VideoEncodeSettings& settings = {});

    /** Apply [settings] to every destination and restart the shared pipeline. */
    bool reconfigure_shared(const VideoEncodeSettings& settings);
    /**
     * Set each destination's encode settings and rebuild the capture → branch ladder.
     * Matching settings merge into one multiudpsink branch (reuse).
     */
    bool apply_branch_layout(
        const VideoEncodeSettings& trunk,
        const std::vector<std::pair<ClientId, VideoEncodeSettings>>& per_client);

    // Legacy dual-stream API — warm probe for trunk replace; commit uses shared restart.
    std::optional<std::string> begin_tier_cutover(
        ClientId client_id,
        const VideoEncodeSettings& settings);
    bool complete_tier_cutover(ClientId client_id, std::string_view staging_video_uri);
    void abort_tier_cutover(ClientId client_id);
    bool cutover_in_flight(ClientId client_id) const;
    std::optional<std::string> current_video_uri(ClientId client_id) const;

    void stop();
    void stop_client(ClientId client_id);

private:
    static constexpr std::uint16_t StagingPortOffset = 128;
    static constexpr std::uint16_t AlternateStagingPortOffset = 129;

    struct Destination {
        ClientId client_id = 0;
        std::string host;
        /** Originally assigned media_index port (low half of the slot block). */
        std::uint16_t base_port = 0;
        /** Port the client currently receives on. */
        std::uint16_t port = 0;
        /** Current encode settings (session ceiling; same for all shared clients). */
        VideoEncodeSettings settings{};
        /** Legacy: leftover dedicated process (stopped on reconfigure_shared). */
        ChildProcess dedicated;
        ChildProcess staging;
        bool staging_active = false;
        std::uint16_t staging_port = 0;
        VideoEncodeSettings staging_settings{};
        std::chrono::steady_clock::time_point staging_started{};
    };

    Destination* find_destination(ClientId client_id);
    const Destination* find_destination(ClientId client_id) const;
    void restart_pipeline();
    void stop_shared_pipeline();
    bool shared_pipeline_running() const;
    std::vector<std::string> build_single_encode_args(
        const VideoEncodeSettings& settings,
        const std::string& host,
        std::uint16_t port) const;
    void apply_nvenc_environment(
        ChildProcess& process,
        std::vector<std::string> args,
        const std::optional<std::string>& stderr_path = std::nullopt);
    /** Staging encodes log to a file so a failed cutover is diagnosable. */
    static std::string staging_encode_log_path();

    enum class SourceKind { X11, PipeWire };
    SourceKind source_kind_ = SourceKind::X11;
    std::string display_;
    std::string pipewire_node_;
    // nvidia-smi index for nvenc via CUDA_VISIBLE_DEVICES; -1 = leave unset.
    int nvenc_cuda_device_id_ = -1;
    std::vector<Destination> destinations_;
    std::unique_ptr<SharedVideoPipelineRunner> shared_pipeline_;
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
    bool reconfigure_shared_video(const VideoEncodeSettings& settings) override;
    bool apply_video_branch_layout(
        const VideoEncodeSettings& trunk,
        const std::vector<std::pair<ClientId, VideoEncodeSettings>>& per_client) override;
    bool restart_shared_audio() override;
    std::optional<std::string> begin_video_tier_cutover(
        ClientId client_id,
        const VideoEncodeSettings& settings) override;
    bool complete_video_tier_cutover(
        ClientId client_id,
        std::string_view staging_video_uri) override;
    void abort_video_tier_cutover(ClientId client_id) override;
    bool video_cutover_in_flight(ClientId client_id) const override;
    std::optional<std::string> current_video_uri(ClientId client_id) const override;
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
