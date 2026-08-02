#include "client/gstreamer_media_receiver.hpp"

#include "client/audio_playback_device.hpp"
#include "client/gstreamer_media_pipeline.hpp"
#include "client/gstreamer_media_platform.hpp"
#include "client/gstreamer_overlay_video.hpp"
#include "client/video_window_geometry.hpp"
#include "common/addresses.hpp"

#include <chrono>
#include <fstream>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace archstreamer {
namespace {

constexpr auto kAudioDevicePollInterval = std::chrono::seconds(5);
// gst-device-monitor on Windows often returns a different "best" id for a frame or two;
// require a stable new key before tearing down the Opus pipeline.
constexpr int kAudioDeviceChangeConfirmPolls = 2;

constexpr auto kPendingGiveUp = std::chrono::seconds(6);
// progressreport ticks once a second while buffers flow; two lines ≈ ~2s of video.
constexpr std::uint64_t kPendingTicksRequired = 2;

std::filesystem::path gst_video_staging_log_path() {
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

} // namespace

GStreamerMediaReceiver::GStreamerMediaReceiver() = default;
GStreamerMediaReceiver::~GStreamerMediaReceiver() {
    disconnect();
}

void GStreamerMediaReceiver::start_audio_pipeline(bool wait_for_ready) {
    if (endpoint_.audio_uri.empty()) {
        return;
    }
    const auto port = audio_port_from_endpoint(endpoint_);
    std::vector<std::string> audio_args{GStreamerMediaPlatform::gst_launch_bin(), "-q"};
    const auto decode = gst_opus_rtp_decode_args(port, 0);
    audio_args.insert(audio_args.end(), decode.begin(), decode.end());

    const auto sink = choose_audio_playback_sink(false);
    bound_audio_device_ = sink.device_key.empty() ? current_audio_playback_device_key() : sink.device_key;
    bound_audio_epoch_ = audio_output_preference_epoch();
    pending_audio_device_.clear();
    pending_audio_device_streak_ = 0;
    audio_pipeline_info_ = sink.description;
    audio_args.insert(audio_args.end(), sink.gst_args.begin(), sink.gst_args.end());
    // Redirect away from the terminal — otherwise Ctrl+C leaves a stuck gst status line
    // and teardown contends with a foreground gst-launch on the same TTY.
    audio_process_.start(audio_args, {}, {}, gst_video_receiver_log_path().string());
    if (wait_for_ready) {
        ensure_gst_child_stayed_up(audio_process_, "Audio", gst_video_receiver_log_path());
    }
}

void GStreamerMediaReceiver::start_video_process(
    ChildProcess& process,
    std::uint16_t port,
    const std::filesystem::path& log_path,
    bool wait_for_ready,
    bool update_pipeline_info) {
    const auto decoder = GStreamerMediaPlatform::choose_h264_decoder();
    auto sink = GStreamerMediaPlatform::choose_video_sink(decoder.d3d11_zero_copy);
    const auto info = std::string("decoder=") + decoder.element + " sink=" + sink.element +
        " log=" + log_path.string();
    if (update_pipeline_info) {
        video_pipeline_info_ = info;
    } else {
        pending_pipeline_info_ = info;
    }

    auto environment = std::vector<std::pair<std::string, std::string>>{};
    auto unset = std::vector<std::string>{};
    GStreamerMediaPlatform::configure_display_for_sink(sink, environment, unset);

    process.start(
        GStreamerMediaPlatform::standalone_video_pipeline(port, decoder, sink, false, std::nullopt),
        environment,
        unset,
        log_path.string());
    if (wait_for_ready) {
        ensure_gst_child_stayed_up(process, "Video", log_path);
    }
}

void GStreamerMediaReceiver::start_overlay_video(std::uint16_t port) {
    if (embed_xid_ == 0 && !video_embed_bridge_) {
        throw std::runtime_error("overlay video requires a Qt video surface");
    }
    if (!gstreamer_overlay_video_available()) {
        throw std::runtime_error(
            "Qt video embed needs GStreamer devel libs (gstreamer-1.0 + "
            "gstreamer-plugins-base); rebuild so ARCHSTREAMER_HAS_GST_LIBS is on");
    }
    if (!overlay_video_) {
        overlay_video_ = std::make_unique<GStreamerOverlayVideo>();
    }
    if (video_embed_bridge_) {
        overlay_video_->set_frame_bridge(video_embed_bridge_);
        video_embed_bridge_->set_emergency_stop([this] {
            if (overlay_video_) {
                // Never wait on the GUI thread — that freezes the X11 event loop.
                overlay_video_->request_stop();
            }
        });
    }
    if (!overlay_video_->start(port, embed_xid_)) {
        if (video_embed_bridge_) {
            video_embed_bridge_->clear_emergency_stop();
        }
        const auto detail = overlay_video_->pipeline_info();
        throw std::runtime_error(
            "in-process video overlay failed" +
            (detail.empty() ? std::string{} : (": " + detail)));
    }
    video_pipeline_info_ = overlay_video_->pipeline_info();
}

bool GStreamerMediaReceiver::primary_video_running() const {
    if ((embed_xid_ != 0 || video_embed_bridge_) && overlay_video_) {
        return overlay_video_->running();
    }
    return video_process_.running();
}

void GStreamerMediaReceiver::set_embed_xid(std::uint64_t xid) {
    embed_xid_ = xid;
}

void GStreamerMediaReceiver::set_video_embed_bridge(std::shared_ptr<VideoEmbedBridge> bridge) {
    video_embed_bridge_ = std::move(bridge);
}

void GStreamerMediaReceiver::connect(const MediaEndpoint& endpoint) {
    disconnect();
    endpoint_ = endpoint;
    video_pipeline_info_.clear();
    audio_pipeline_info_.clear();
    bound_audio_device_.clear();
    pending_audio_device_.clear();
    pending_audio_device_streak_ = 0;
    next_audio_device_check_ = std::chrono::steady_clock::now() + kAudioDevicePollInterval;

    // Start audio before video so the Opus path is bound before the first
    // video frames appear (reduces the common "picture first, sound later" skew).
    if (!endpoint_.audio_uri.empty()) {
        start_audio_pipeline(true);
    }

    if (!endpoint_.video_uri.empty()) {
        const auto port = video_port_from_endpoint(endpoint_);
        if (embed_xid_ != 0 || video_embed_bridge_) {
            start_overlay_video(port);
        } else {
            start_video_process(video_process_, port, gst_video_receiver_log_path(), true, true);
        }
    }
}

void GStreamerMediaReceiver::disconnect() {
    abort_pending_video();
    if (video_embed_bridge_) {
        video_embed_bridge_->clear_emergency_stop();
    }
    audio_process_.stop();
    video_process_.stop();
    if (overlay_video_) {
        // Kick the pipeline to NULL without waiting on this thread — a multi-second
        // gst_element_get_state wait here stalls the desktop (GPU/decoder drain).
        overlay_video_->request_stop();
        auto doomed = std::move(overlay_video_);
        std::thread([pipeline = std::move(doomed)]() mutable {
            if (pipeline) {
                pipeline->stop();
                pipeline.reset();
            }
        }).detach();
    }
    endpoint_ = {};
    bound_audio_device_.clear();
    pending_audio_device_.clear();
    pending_audio_device_streak_ = 0;
    audio_pipeline_info_.clear();
    video_pipeline_info_.clear();
}

bool GStreamerMediaReceiver::begin_pending_video(const std::string& video_uri) {
    if (embed_xid_ != 0 || video_embed_bridge_) {
        return begin_embed_staging_probe(video_uri);
    }
    if (video_uri.empty() || pending_active_ || !video_process_.running()) {
        return false;
    }
    MediaEndpoint staging_endpoint;
    staging_endpoint.video_uri = video_uri;
    const auto port = video_port_from_endpoint(staging_endpoint);
    if (port == 0) {
        return false;
    }

    // One snapshot for reveal/promote — do not keep re-applying while warming
    // (that fights the user's drag and makes the window "dance").
    pending_target_geometry_ = capture_video_window_geometry(video_process_.pid());
    pending_offscreen_parked_ = false;

    const auto log_path = gst_video_staging_log_path();
    std::error_code error;
    std::filesystem::remove(log_path, error);
    pending_video_.stop();
    try {
        start_video_process(pending_video_, port, log_path, false, false);
    } catch (const std::exception&) {
        pending_video_.stop();
        return false;
    }
    if (!pending_video_.running()) {
        return false;
    }
    pending_video_uri_ = video_uri;
    pending_started_ = std::chrono::steady_clock::now();
    pending_active_ = true;
    pending_ack_sent_ = false;
    embed_probe_mode_ = false;
    try_park_pending_offscreen();
    return true;
}

bool GStreamerMediaReceiver::begin_embed_staging_probe(const std::string& video_uri) {
    if (video_uri.empty() || pending_active_ || !primary_video_running()) {
        return false;
    }
    MediaEndpoint staging_endpoint;
    staging_endpoint.video_uri = video_uri;
    const auto port = video_port_from_endpoint(staging_endpoint);
    if (port == 0) {
        return false;
    }

    const auto log_path = gst_video_staging_log_path();
    std::error_code error;
    std::filesystem::remove(log_path, error);
    pending_video_.stop();

    // Headless probe — Qt window keeps showing the old pipeline until cold switch.
    auto args = std::vector<std::string>{GStreamerMediaPlatform::gst_launch_bin()};
    auto source = gst_h264_rtp_source_args(port);
    args.insert(args.end(), source.begin(), source.end());
    gst_append_h264parse_if_available(args);
    args.insert(args.end(), {"progressreport", "update-freq=1", "!", "fakesink", "sync=false"});
    try {
        pending_video_.start(args, {}, {}, log_path.string());
    } catch (const std::exception&) {
        pending_video_.stop();
        return false;
    }
    if (!pending_video_.running()) {
        return false;
    }
    pending_video_uri_ = video_uri;
    pending_started_ = std::chrono::steady_clock::now();
    pending_active_ = true;
    pending_ack_sent_ = false;
    embed_probe_mode_ = true;
    pending_offscreen_parked_ = true;
    return true;
}

void GStreamerMediaReceiver::refresh_pending_target_from_primary() {
    if (!video_process_.running()) {
        return;
    }
    if (const auto live = capture_video_window_geometry(video_process_.pid()); live.valid) {
        pending_target_geometry_ = live;
    }
}

void GStreamerMediaReceiver::try_park_pending_offscreen() {
    if (pending_offscreen_parked_ || !pending_active_ || !pending_video_.running()) {
        return;
    }
    VideoWindowGeometry park{};
    park.valid = true;
    park.x = -12800;
    park.y = -12800;
    if (pending_target_geometry_.valid) {
        park.width = pending_target_geometry_.width;
        park.height = pending_target_geometry_.height;
    } else {
        park.width = 1280;
        park.height = 720;
    }
    // Keep normal (not max/FS) while hidden so reveal can apply the real state once.
    if (apply_video_window_geometry(
            pending_video_.pid(),
            park,
            std::chrono::milliseconds(50))) {
        pending_offscreen_parked_ = true;
    }
}

bool GStreamerMediaReceiver::reveal_pending_on_target(std::chrono::milliseconds timeout) {
    refresh_pending_target_from_primary();
    if (!pending_target_geometry_.valid || !pending_video_.running()) {
        return false;
    }
    if (!apply_video_window_geometry(
            pending_video_.pid(),
            pending_target_geometry_,
            timeout)) {
        return false;
    }
    raise_video_window(pending_video_.pid());
    return true;
}

std::optional<std::string> GStreamerMediaReceiver::poll_pending_ready() {
    if (!pending_active_) {
        return std::nullopt;
    }
    if (embed_probe_mode_) {
        return poll_embed_staging_probe();
    }
    if (!pending_video_.running()) {
        abort_pending_video();
        return std::nullopt;
    }
    if (pending_ack_sent_) {
        return std::nullopt;
    }

    try_park_pending_offscreen();
    // Update target quietly for reveal/promote; do not move the pending window yet.
    refresh_pending_target_from_primary();

    if (progress_tick_count(gst_video_staging_log_path()) < kPendingTicksRequired) {
        if (std::chrono::steady_clock::now() - pending_started_ >= kPendingGiveUp) {
            abort_pending_video();
        }
        return std::nullopt;
    }

    // One on-screen move + raise, then ACK. Host may kill the old encode next.
    reveal_pending_on_target(std::chrono::milliseconds(500));
    pending_ack_sent_ = true;
    return pending_video_uri_;
}

std::optional<std::string> GStreamerMediaReceiver::poll_embed_staging_probe() {
    if (!pending_video_.running()) {
        abort_pending_video();
        return std::nullopt;
    }
    if (pending_ack_sent_) {
        return std::nullopt;
    }
    if (progress_tick_count(gst_video_staging_log_path()) < kPendingTicksRequired) {
        if (std::chrono::steady_clock::now() - pending_started_ >= kPendingGiveUp) {
            abort_pending_video();
        }
        return std::nullopt;
    }
    // Free the staging port; cold switch_video into the Qt window happens on MediaEndpoint.
    auto acked = pending_video_uri_;
    abort_pending_video();
    return acked;
}

bool GStreamerMediaReceiver::pending_video_active() const {
    return pending_active_;
}

void GStreamerMediaReceiver::abort_pending_video() {
    pending_video_.stop();
    pending_active_ = false;
    pending_ack_sent_ = false;
    pending_offscreen_parked_ = false;
    embed_probe_mode_ = false;
    pending_video_uri_.clear();
    pending_pipeline_info_.clear();
    pending_target_geometry_ = {};
}

bool GStreamerMediaReceiver::promote_or_switch_video(const std::string& video_uri) {
    if (video_uri.empty()) {
        return false;
    }
    // Embed path: always cold-restart into the same Qt window (no second sink).
    if (embed_xid_ != 0 || video_embed_bridge_) {
        abort_pending_video();
        return switch_video(video_uri);
    }
    if (pending_active_ && pending_video_uri_ == video_uri && pending_video_.running()) {
        refresh_pending_target_from_primary();
        const auto geometry = pending_target_geometry_;

        video_process_.stop();
        video_process_ = std::move(pending_video_);
        endpoint_.video_uri = video_uri;
        video_pipeline_info_ = std::move(pending_pipeline_info_);
        pending_active_ = false;
        pending_ack_sent_ = false;
        pending_offscreen_parked_ = false;
        pending_video_uri_.clear();
        pending_target_geometry_ = {};

        if (geometry.valid) {
            apply_video_window_geometry(video_process_.pid(), geometry);
        }
        raise_video_window(video_process_.pid());
        return video_process_.running();
    }
    abort_pending_video();
    return switch_video(video_uri);
}

bool GStreamerMediaReceiver::switch_video(const std::string& video_uri) {
    if (video_uri.empty()) {
        return false;
    }
    MediaEndpoint next = endpoint_;
    next.video_uri = video_uri;
    const auto port = video_port_from_endpoint(next);
    if (port == 0) {
        return false;
    }

    if (embed_xid_ != 0 || video_embed_bridge_) {
        try {
            if (!overlay_video_) {
                start_overlay_video(port);
            } else if (!overlay_video_->switch_port(port)) {
                const auto detail = overlay_video_->pipeline_info();
                throw std::runtime_error(
                    "overlay switch_port failed" +
                    (detail.empty() ? std::string{} : (": " + detail)));
            } else {
                video_pipeline_info_ = overlay_video_->pipeline_info();
            }
        } catch (const std::exception&) {
            return false;
        }
        endpoint_.video_uri = video_uri;
        return primary_video_running();
    }

    VideoWindowGeometry geometry = capture_video_window_geometry(video_process_.pid());
    video_process_.stop();
    try {
        start_video_process(video_process_, port, gst_video_receiver_log_path(), true, true);
    } catch (const std::exception&) {
        return false;
    }
    if (geometry.valid) {
        apply_video_window_geometry(video_process_.pid(), geometry);
    }
    endpoint_.video_uri = video_uri;
    return video_process_.running();
}

bool GStreamerMediaReceiver::poll() {
    if (endpoint_.audio_uri.empty()) {
        return false;
    }
    const auto epoch = audio_output_preference_epoch();
    const bool preference_changed = epoch != bound_audio_epoch_;
    const auto now = std::chrono::steady_clock::now();
    if (!preference_changed && now < next_audio_device_check_) {
        return false;
    }
    next_audio_device_check_ = now + kAudioDevicePollInterval;

    // Do not rebind on gst-device-monitor "best device" churn — on Windows that
    // intermittently restarts Opus and sounds like dropouts. Only follow an
    // explicit Settings preference change, or recover if the process died.
    const bool died = !audio_process_.running();
    if (!preference_changed && !died) {
        return false;
    }

    return restart_audio();
}

bool GStreamerMediaReceiver::restart_audio() {
    if (endpoint_.audio_uri.empty()) {
        return false;
    }
    audio_process_.stop();
    start_audio_pipeline(false);
    return audio_process_.running();
}

bool GStreamerMediaReceiver::video_running() const {
    // Pending counts: after ACK the host may kill the old encode and the primary
    // gst-launch can exit while the warm staging window is still the live picture.
    return primary_video_running() || pending_video_.running();
}

bool GStreamerMediaReceiver::audio_running() const {
    return audio_process_.running();
}

const std::string& GStreamerMediaReceiver::video_pipeline_info() const {
    return video_pipeline_info_;
}

const std::string& GStreamerMediaReceiver::audio_pipeline_info() const {
    return audio_pipeline_info_;
}

bool GStreamerMediaReceiver::video_frames_seen() const {
    return decoded_frame_count() > 0;
}

std::uint64_t GStreamerMediaReceiver::decoded_frame_count() const {
    if ((embed_xid_ != 0 || video_embed_bridge_) && overlay_video_) {
        return overlay_video_->frames_seen();
    }
    const auto marker = video_pipeline_info_.find("log=");
    if (marker == std::string::npos) {
        return 0;
    }
    const auto path = video_pipeline_info_.substr(marker + 4);
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

void GStreamerMediaReceiver::apply_video_overlay_geometry(int width, int height) {
    if (width > 0 && height > 0) {
        overlay_width_ = width;
        overlay_height_ = height;
    }
    if (overlay_video_) {
        overlay_video_->set_render_size(width, height);
    }
}

void GStreamerMediaReceiver::expose_video_overlay() {
    if (overlay_video_) {
        overlay_video_->expose();
    }
}

} // namespace archstreamer
