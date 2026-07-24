#include "client/gstreamer_synced_media_session.hpp"

#include "client/audio_playback_device.hpp"
#include "client/gstreamer_media_pipeline.hpp"
#include "client/gstreamer_media_platform.hpp"
#include "common/addresses.hpp"

#include <chrono>
#include <fstream>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace archstreamer {
namespace {

constexpr auto kAudioDevicePollInterval = std::chrono::seconds(2);

void append_audio_branch(
    std::vector<std::string>& args,
    std::uint16_t port,
    const AudioPlaybackSink& sink) {
    const auto decode = gst_opus_rtp_decode_args(port, 80);
    args.insert(args.end(), decode.begin(), decode.end());
    args.insert(args.end(), sink.gst_args.begin(), sink.gst_args.end());
}

} // namespace

GStreamerSyncedMediaSession::GStreamerSyncedMediaSession() {
    video_.session_ = this;
    audio_.session_ = this;
}

GStreamerSyncedMediaSession::~GStreamerSyncedMediaSession() {
    disconnect();
}

bool GStreamerSyncedMediaSession::VideoBranch::running() const {
    return enabled_ && session_ != nullptr && session_->running();
}

bool GStreamerSyncedMediaSession::AudioBranch::running() const {
    return enabled_ && session_ != nullptr && session_->running();
}

bool GStreamerSyncedMediaSession::running() const {
    return process_.running();
}

void GStreamerSyncedMediaSession::connect(const MediaEndpoint& endpoint) {
    disconnect();

    const bool want_video = !endpoint.video_uri.empty();
    const bool want_audio = !endpoint.audio_uri.empty();
    if (!want_video && !want_audio) {
        return;
    }

    auto args = std::vector<std::string>{GStreamerMediaPlatform::gst_launch_bin(), "-q"};
    auto environment = std::vector<std::pair<std::string, std::string>>{};
    auto unset = std::vector<std::string>{};
    stderr_log_path_ = gst_synced_receiver_log_path().string();

    if (want_video) {
        const auto decoder = GStreamerMediaPlatform::choose_h264_decoder();
        const auto sink = GStreamerMediaPlatform::choose_video_sink(decoder.d3d11_zero_copy);
        video_.enabled_ = true;
        video_.pipeline_info_ = std::string("synced decoder=") + decoder.element +
            " sink=" + sink.element + " sync=true log=" + stderr_log_path_;
        GStreamerMediaPlatform::append_video_branch(
            args,
            video_port_from_endpoint(endpoint),
            decoder,
            sink,
            true);
        GStreamerMediaPlatform::configure_display_for_sink(sink, environment, unset);
    }

    if (want_audio) {
        const auto sink = choose_audio_playback_sink(true);
        audio_.enabled_ = true;
        audio_.pipeline_info_ = "synced " + sink.description;
        append_audio_branch(args, audio_port_from_endpoint(endpoint), sink);
    }

    process_.start(std::move(args), environment, unset, stderr_log_path_);
    ensure_gst_child_stayed_up(process_, "Synced media", stderr_log_path_);
}

void GStreamerSyncedMediaSession::disconnect() {
    process_.stop();
    video_.enabled_ = false;
    audio_.enabled_ = false;
    video_.pipeline_info_.clear();
    audio_.pipeline_info_.clear();
    stderr_log_path_.clear();
}

std::uint64_t GStreamerSyncedMediaSession::decoded_frame_count() const {
    if (stderr_log_path_.empty()) {
        return 0;
    }
    std::ifstream in(stderr_log_path_);
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

bool GStreamerSyncedMediaSession::video_frames_seen() const {
    return decoded_frame_count() > 0;
}

void GStreamerSyncedMediaReceiver::connect(const MediaEndpoint& endpoint) {
    endpoint_ = endpoint;
    bound_audio_device_ = endpoint.audio_uri.empty()
        ? std::string{}
        : current_audio_playback_device_key();
    next_audio_device_check_ = std::chrono::steady_clock::now() + kAudioDevicePollInterval;
    session_.connect(endpoint_);
}

void GStreamerSyncedMediaReceiver::disconnect() {
    session_.disconnect();
    endpoint_ = {};
    bound_audio_device_.clear();
}

bool GStreamerSyncedMediaReceiver::poll() {
    if (endpoint_.audio_uri.empty()) {
        return false;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now < next_audio_device_check_) {
        return false;
    }
    next_audio_device_check_ = now + kAudioDevicePollInterval;

    const auto device = current_audio_playback_device_key();
    const bool device_changed = !device.empty() && device != bound_audio_device_;
    const bool died = !session_.running();
    if (!device_changed && !died) {
        return false;
    }

    session_.disconnect();
    bound_audio_device_ = device;
    session_.connect(endpoint_);
    return true;
}

bool GStreamerSyncedMediaReceiver::video_running() const {
    return session_.video().running();
}

bool GStreamerSyncedMediaReceiver::audio_running() const {
    return session_.audio().running();
}

bool GStreamerSyncedMediaReceiver::video_frames_seen() const {
    return session_.video_frames_seen();
}

std::uint64_t GStreamerSyncedMediaReceiver::decoded_frame_count() const {
    return session_.decoded_frame_count();
}

const std::string& GStreamerSyncedMediaReceiver::video_pipeline_info() const {
    return session_.video().pipeline_info();
}

const std::string& GStreamerSyncedMediaReceiver::audio_pipeline_info() const {
    return session_.audio().pipeline_info();
}

} // namespace archstreamer
