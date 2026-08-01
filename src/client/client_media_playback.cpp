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

constexpr auto kProbeGiveUp = std::chrono::seconds(6);
// progressreport ticks once a second and only while buffers flow, so two lines
// means the staging port carried real video for ~2s.
constexpr std::uint64_t kProbeTicksRequired = 2;

std::filesystem::path staging_probe_log_path() {
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

// No decoder and no sink window: arriving, depayloadable H.264 is all the proof
// the ACK needs, and it costs nothing to run beside the playing pipeline.
std::vector<std::string> probe_pipeline_args(std::uint16_t port) {
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
    end_staging_probe();
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
    if (video_uri.empty() || staging_active_ || !active()) {
        return false;
    }
    MediaEndpoint staging_endpoint;
    staging_endpoint.video_uri = video_uri;
    const auto port = video_port_from_endpoint(staging_endpoint);
    if (port == 0) {
        return false;
    }

    const auto log_path = staging_probe_log_path();
    std::error_code error;
    std::filesystem::remove(log_path, error);
    staging_probe_.stop();
    try {
        staging_probe_.start(probe_pipeline_args(port), {}, {}, log_path.string());
    } catch (const std::exception&) {
        staging_probe_.stop();
        return false;
    }
    if (!staging_probe_.running()) {
        return false;
    }
    pending_video_uri_ = video_uri;
    staging_started_ = std::chrono::steady_clock::now();
    staging_active_ = true;
    return true;
}

std::optional<std::string> ClientMediaPlayback::poll_video_cutover() {
    if (!staging_active_) {
        return std::nullopt;
    }
    if (!staging_probe_.running()) {
        end_staging_probe();
        return std::nullopt;
    }
    // A udpsrc sits at PLAYING forever on a port nobody sends to, so process
    // health proves nothing — only ticks do.
    if (progress_tick_count(staging_probe_log_path()) < kProbeTicksRequired) {
        if (std::chrono::steady_clock::now() - staging_started_ >= kProbeGiveUp) {
            end_staging_probe();
        }
        return std::nullopt;
    }

    auto acked = pending_video_uri_;
    // Free the port before the host promotes; the playing pipeline moves there
    // only once the host answers with an updated endpoint.
    end_staging_probe();
    return acked;
}

bool ClientMediaPlayback::video_cutover_pending() const {
    return staging_active_;
}

bool ClientMediaPlayback::switch_video(const std::string& video_uri) {
    if (video_uri.empty() || video_uri == endpoint_.video_uri) {
        return false;
    }
    if (legacy_) {
        const bool moved = legacy_->switch_video(video_uri);
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

void ClientMediaPlayback::end_staging_probe() {
    staging_probe_.stop();
    staging_active_ = false;
    pending_video_uri_.clear();
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
