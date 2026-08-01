#pragma once

#include "client/media_receiver.hpp"
#include "common/media.hpp"
#include "common/platform/default_platform.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace archstreamer {

class GStreamerMediaReceiver;
class GStreamerSyncedMediaReceiver;

// Owns the legacy dual-process and experimental synced single-process receivers.
// Call sites pick a strategy; diagnostics stay on this facade so either path can be
// swapped or A/B'd without rewriting ClientApp / Watch-local.
class ClientMediaPlayback final : public MediaReceiver {
public:
    enum class Strategy {
        Legacy,
        Synced,
    };

    ClientMediaPlayback();
    ~ClientMediaPlayback() override;

    ClientMediaPlayback(ClientMediaPlayback&&) noexcept;
    ClientMediaPlayback& operator=(ClientMediaPlayback&&) noexcept;

    ClientMediaPlayback(const ClientMediaPlayback&) = delete;
    ClientMediaPlayback& operator=(const ClientMediaPlayback&) = delete;

    void connect(const MediaEndpoint& endpoint) override;
    void connect(const MediaEndpoint& endpoint, Strategy strategy);
    void disconnect() override;
    bool poll() override;

    /** Tear down and reconnect both A/V branches (Synced path, or hard recovery). */
    bool resync();

    /**
     * Realign lip-sync on the Legacy path: restart audio only so it meets the
     * current live video edge (typical case: video stalled/behind, audio ran ahead).
     * Synced path falls back to a full resync.
     */
    bool resync_audio();

    Strategy strategy() const { return strategy_; }
    bool active() const;
    bool has_endpoint() const;

    bool video_running() const;
    bool audio_running() const;
    bool video_frames_seen() const;
    std::uint64_t decoded_frame_count() const;
    const std::string& video_pipeline_info() const;
    const std::string& audio_pipeline_info() const;

    const MediaEndpoint& endpoint() const { return endpoint_; }

    /**
     * Start a headless probe on the host's staging RTP port. The playing
     * pipeline is left completely alone: a cutover must never cost the picture
     * we already have.
     */
    bool begin_video_pending(const std::string& video_uri);
    /**
     * Returns the staging URI to ACK once the probe proves the host is really
     * publishing there, and nothing until then. Gives up silently on timeout.
     */
    std::optional<std::string> poll_video_cutover();
    bool video_cutover_pending() const;

    /** Move the playing video to a new port after the host confirms the swap. */
    bool switch_video(const std::string& video_uri);

    explicit operator bool() const { return active(); }

private:
    void end_staging_probe();

    Strategy strategy_ = Strategy::Legacy;
    MediaEndpoint endpoint_{};
    bool has_endpoint_ = false;
    ChildProcess staging_probe_;
    std::string pending_video_uri_;
    std::chrono::steady_clock::time_point staging_started_{};
    bool staging_active_ = false;
    std::unique_ptr<GStreamerMediaReceiver> legacy_;
    std::unique_ptr<GStreamerSyncedMediaReceiver> synced_;
};

} // namespace archstreamer
