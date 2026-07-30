#pragma once

#include "client/media_receiver.hpp"
#include "common/media.hpp"

#include <cstdint>
#include <memory>
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

    /** Tear down and reconnect both A/V branches with the last endpoint (mid-session lip-sync recovery). */
    bool resync();

    Strategy strategy() const { return strategy_; }
    bool active() const;
    bool has_endpoint() const;

    bool video_running() const;
    bool audio_running() const;
    bool video_frames_seen() const;
    std::uint64_t decoded_frame_count() const;
    const std::string& video_pipeline_info() const;
    const std::string& audio_pipeline_info() const;

    explicit operator bool() const { return active(); }

private:
    Strategy strategy_ = Strategy::Legacy;
    MediaEndpoint endpoint_{};
    bool has_endpoint_ = false;
    std::unique_ptr<GStreamerMediaReceiver> legacy_;
    std::unique_ptr<GStreamerSyncedMediaReceiver> synced_;
};

} // namespace archstreamer
