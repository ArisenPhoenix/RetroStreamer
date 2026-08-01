#include "client/gstreamer_media_receiver.hpp"

#include "client/audio_playback_device.hpp"
#include "client/gstreamer_media_pipeline.hpp"
#include "client/gstreamer_media_platform.hpp"
#include "client/video_window_geometry.hpp"
#include "common/addresses.hpp"

#include <chrono>
#include <fstream>
#include <utility>
#include <vector>

namespace archstreamer {
namespace {

constexpr auto kAudioDevicePollInterval = std::chrono::seconds(5);
// gst-device-monitor on Windows often returns a different "best" id for a frame or two;
// require a stable new key before tearing down the Opus pipeline.
constexpr int kAudioDeviceChangeConfirmPolls = 2;

} // namespace

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
    audio_process_.start(audio_args);
    if (wait_for_ready) {
        ensure_gst_child_stayed_up(audio_process_, "Audio", gst_video_receiver_log_path());
    }
}

void GStreamerMediaReceiver::start_video_process(
    ChildProcess& process,
    std::uint16_t port,
    const std::filesystem::path& log_path,
    bool wait_for_ready) {
    const auto decoder = GStreamerMediaPlatform::choose_h264_decoder();
    const auto sink = GStreamerMediaPlatform::choose_video_sink(decoder.d3d11_zero_copy);
    video_pipeline_info_ = std::string("decoder=") + decoder.element + " sink=" + sink.element +
        " log=" + log_path.string();

    auto environment = std::vector<std::pair<std::string, std::string>>{};
    auto unset = std::vector<std::string>{};
    GStreamerMediaPlatform::configure_display_for_sink(sink, environment, unset);

    process.start(
        GStreamerMediaPlatform::standalone_video_pipeline(port, decoder, sink, false),
        environment,
        unset,
        log_path.string());
    if (wait_for_ready) {
        ensure_gst_child_stayed_up(process, "Video", log_path);
    }
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
        start_video_process(video_process_, port, gst_video_receiver_log_path(), true);
    }
}

void GStreamerMediaReceiver::disconnect() {
    audio_process_.stop();
    video_process_.stop();
    endpoint_ = {};
    bound_audio_device_.clear();
    pending_audio_device_.clear();
    pending_audio_device_streak_ = 0;
    audio_pipeline_info_.clear();
    video_pipeline_info_.clear();
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

    // Snapshot before tear-down so the replacement window can land in the same place.
    const auto geometry = capture_video_window_geometry(video_process_.pid());
    video_process_.stop();
    try {
        start_video_process(video_process_, port, gst_video_receiver_log_path(), true);
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
    return video_process_.running();
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

} // namespace archstreamer
