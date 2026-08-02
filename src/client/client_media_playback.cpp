#include "client/client_media_playback.hpp"

#include "client/gstreamer_media_receiver.hpp"
#include "client/gstreamer_media_pipeline.hpp"
#include "client/gstreamer_media_platform.hpp"
#include "client/gstreamer_synced_media_session.hpp"
#include "common/addresses.hpp"

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace archstreamer {
namespace {

constexpr auto kSyncedProbeGiveUp = std::chrono::seconds(6);
constexpr std::uint64_t kSyncedProbeTicksRequired = 2;

std::filesystem::path synced_staging_probe_log_path() {
    return gst_video_receiver_log_path().parent_path() / "gst-video-staging.log";
}

std::uint64_t progress_tick_count(const std::filesystem::path& path) {
    std::ifstream in(path);
    if (!in) {
        return 0;
    }
    std::uint64_t count = 0;
    std::string line;
    while (std::getline(in, line)) {
        if (line.find("progressreport") != std::string::npos) {
            ++count;
        }
    }
    return count;
}

// Synced cutover still uses a headless probe; warm dual windows need a second
// display sink the single-process session does not own yet.
std::vector<std::string> synced_probe_pipeline_args(std::uint16_t port) {
    auto args = std::vector<std::string>{GStreamerMediaPlatform::gst_launch_bin()};
    auto source = gst_h264_rtp_source_args(port);
    args.insert(args.end(), source.begin(), source.end());
    gst_append_h264parse_if_available(args);
    args.insert(args.end(), {"progressreport", "update-freq=1", "!", "fakesink", "sync=false"});
    return args;
}

} // namespace

ClientMediaPlayback::ClientMediaPlayback() = default;
ClientMediaPlayback::~ClientMediaPlayback() = default;
ClientMediaPlayback::ClientMediaPlayback(ClientMediaPlayback&&) noexcept = default;
ClientMediaPlayback& ClientMediaPlayback::operator=(ClientMediaPlayback&&) noexcept = default;

void ClientMediaPlayback::connect(const MediaEndpoint& endpoint) {
    connect(endpoint, strategy_, 0, nullptr);
}

void ClientMediaPlayback::connect(const MediaEndpoint& endpoint, Strategy strategy) {
    connect(endpoint, strategy, 0, nullptr);
}

void ClientMediaPlayback::connect(
    const MediaEndpoint& endpoint,
    Strategy strategy,
    std::uint64_t video_embed_xid) {
    connect(endpoint, strategy, video_embed_xid, nullptr);
}

void ClientMediaPlayback::connect(
    const MediaEndpoint& endpoint,
    Strategy strategy,
    std::uint64_t video_embed_xid,
    std::shared_ptr<VideoEmbedBridge> video_embed) {
    disconnect();
    strategy_ = strategy;
    endpoint_ = endpoint;
    has_endpoint_ = !endpoint_.video_uri.empty() || !endpoint_.audio_uri.empty();
    if (!has_endpoint_) {
        return;
    }
    if (strategy_ == Strategy::Synced) {
        // Synced single-process path does not support Qt embed yet.
        synced_ = std::make_unique<GStreamerSyncedMediaReceiver>();
        synced_->connect(endpoint_);
        return;
    }
    legacy_ = std::make_unique<GStreamerMediaReceiver>();
    if (video_embed) {
        legacy_->set_video_embed_bridge(std::move(video_embed));
    }
    if (video_embed_xid != 0) {
        legacy_->set_embed_xid(video_embed_xid);
    }
    legacy_->connect(endpoint_);
}

void ClientMediaPlayback::disconnect() {
    end_synced_staging_probe();
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

bool ClientMediaPlayback::begin_video_pending(const std::string& video_uri) {
    if (legacy_) {
        return legacy_->begin_pending_video(video_uri);
    }
    if (!synced_ || video_uri.empty() || synced_staging_active_) {
        return false;
    }
    MediaEndpoint staging_endpoint;
    staging_endpoint.video_uri = video_uri;
    const auto port = video_port_from_endpoint(staging_endpoint);
    if (port == 0) {
        return false;
    }

    const auto log_path = synced_staging_probe_log_path();
    std::error_code error;
    std::filesystem::remove(log_path, error);
    synced_staging_probe_.stop();
    try {
        synced_staging_probe_.start(synced_probe_pipeline_args(port), {}, {}, log_path.string());
    } catch (const std::exception&) {
        synced_staging_probe_.stop();
        return false;
    }
    if (!synced_staging_probe_.running()) {
        return false;
    }
    synced_pending_video_uri_ = video_uri;
    synced_staging_started_ = std::chrono::steady_clock::now();
    synced_staging_active_ = true;
    return true;
}

std::optional<std::string> ClientMediaPlayback::poll_video_cutover() {
    if (legacy_) {
        return legacy_->poll_pending_ready();
    }
    if (!synced_staging_active_) {
        return std::nullopt;
    }
    if (!synced_staging_probe_.running()) {
        end_synced_staging_probe();
        return std::nullopt;
    }
    if (progress_tick_count(synced_staging_probe_log_path()) < kSyncedProbeTicksRequired) {
        if (std::chrono::steady_clock::now() - synced_staging_started_ >= kSyncedProbeGiveUp) {
            end_synced_staging_probe();
        }
        return std::nullopt;
    }

    auto acked = synced_pending_video_uri_;
    // Free the staging port before Synced reconnect binds it.
    end_synced_staging_probe();
    return acked;
}

bool ClientMediaPlayback::video_cutover_pending() const {
    if (legacy_) {
        return legacy_->pending_video_active();
    }
    return synced_staging_active_;
}

bool ClientMediaPlayback::switch_video(const std::string& video_uri) {
    if (video_uri.empty() || video_uri == endpoint_.video_uri) {
        return false;
    }
    if (legacy_) {
        const bool moved = legacy_->promote_or_switch_video(video_uri);
        // Even on failure the host has already dropped the old encode, so resync
        // has to aim at the new port rather than the one that is now silent.
        endpoint_.video_uri = video_uri;
        return moved;
    }
    if (synced_) {
        // Single process: the only way to move video is to rebuild both branches.
        // Geometry is captured inside the legacy path only; synced reconnect gets
        // a fresh default window until in-process playback lands.
        endpoint_.video_uri = video_uri;
        connect(endpoint_, Strategy::Synced);
        return active();
    }
    return false;
}

void ClientMediaPlayback::end_synced_staging_probe() {
    synced_staging_probe_.stop();
    synced_staging_active_ = false;
    synced_pending_video_uri_.clear();
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

void ClientMediaPlayback::apply_video_overlay_geometry(int width, int height) {
    if (legacy_) {
        legacy_->apply_video_overlay_geometry(width, height);
    }
}

void ClientMediaPlayback::expose_video_overlay() {
    if (legacy_) {
        legacy_->expose_video_overlay();
    }
}

} // namespace archstreamer
