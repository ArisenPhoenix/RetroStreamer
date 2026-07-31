#include "client/client_media_playback.hpp"

#include "client/gstreamer_media_receiver.hpp"
#include "client/gstreamer_synced_media_session.hpp"

namespace archstreamer {

ClientMediaPlayback::ClientMediaPlayback() = default;
ClientMediaPlayback::~ClientMediaPlayback() = default;
ClientMediaPlayback::ClientMediaPlayback(ClientMediaPlayback&&) noexcept = default;
ClientMediaPlayback& ClientMediaPlayback::operator=(ClientMediaPlayback&&) noexcept = default;

void ClientMediaPlayback::connect(const MediaEndpoint& endpoint) {
    connect(endpoint, strategy_);
}

void ClientMediaPlayback::connect(const MediaEndpoint& endpoint, Strategy strategy) {
    disconnect();
    strategy_ = strategy;
    endpoint_ = endpoint;
    has_endpoint_ = !endpoint_.video_uri.empty() || !endpoint_.audio_uri.empty();
    if (!has_endpoint_) {
        return;
    }
    if (strategy_ == Strategy::Synced) {
        synced_ = std::make_unique<GStreamerSyncedMediaReceiver>();
        synced_->connect(endpoint_);
        return;
    }
    legacy_ = std::make_unique<GStreamerMediaReceiver>();
    legacy_->connect(endpoint_);
}

void ClientMediaPlayback::disconnect() {
    if (legacy_) {
        legacy_->disconnect();
    }
    if (synced_) {
        synced_->disconnect();
    }
    legacy_.reset();
    synced_.reset();
    // Keep endpoint_/strategy_ so resync() can reconnect after a transient stall.
}

bool ClientMediaPlayback::resync() {
    if (!has_endpoint_) {
        return false;
    }
    connect(endpoint_, strategy_);
    return active();
}

bool ClientMediaPlayback::resync_audio() {
    if (!has_endpoint_) {
        return false;
    }
    if (synced_) {
        // Single process — cannot restart Opus alone.
        return resync();
    }
    if (!legacy_) {
        return false;
    }
    return legacy_->restart_audio();
}

bool ClientMediaPlayback::poll() {
    if (synced_) {
        // Synced path already restarts the whole session on device/death.
        return synced_->poll();
    }
    // Legacy: audio-only rebind (device change / died). Leaving video running is
    // intentional — restarting Opus is how we pull audio back to the live edge.
    return legacy_ && legacy_->poll();
}

bool ClientMediaPlayback::active() const {
    return legacy_ != nullptr || synced_ != nullptr;
}

bool ClientMediaPlayback::has_endpoint() const {
    return has_endpoint_;
}

bool ClientMediaPlayback::video_running() const {
    if (synced_) {
        return synced_->video_running();
    }
    return legacy_ && legacy_->video_running();
}

bool ClientMediaPlayback::audio_running() const {
    if (synced_) {
        return synced_->audio_running();
    }
    return legacy_ && legacy_->audio_running();
}

bool ClientMediaPlayback::video_frames_seen() const {
    if (synced_) {
        return synced_->video_frames_seen();
    }
    return legacy_ && legacy_->video_frames_seen();
}

std::uint64_t ClientMediaPlayback::decoded_frame_count() const {
    if (synced_) {
        return synced_->decoded_frame_count();
    }
    return legacy_ ? legacy_->decoded_frame_count() : 0;
}

const std::string& ClientMediaPlayback::video_pipeline_info() const {
    if (synced_) {
        return synced_->video_pipeline_info();
    }
    static const std::string empty;
    return legacy_ ? legacy_->video_pipeline_info() : empty;
}

const std::string& ClientMediaPlayback::audio_pipeline_info() const {
    if (synced_) {
        return synced_->audio_pipeline_info();
    }
    static const std::string empty;
    return legacy_ ? legacy_->audio_pipeline_info() : empty;
}

} // namespace archstreamer
