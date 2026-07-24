#include "client/gstreamer_media_receiver.hpp"

#include "client/audio_playback_device.hpp"
#include "client/gstreamer_media_pipeline.hpp"
#include "client/gstreamer_media_platform.hpp"
#include "common/addresses.hpp"

#include <chrono>
#include <fstream>
#include <utility>
#include <vector>

namespace archstreamer {
namespace {

constexpr auto kAudioDevicePollInterval = std::chrono::seconds(2);

} // namespace

void GStreamerMediaReceiver::start_audio_pipeline(bool wait_for_ready) {
    if (endpoint_.audio_uri.empty()) {
        return;
    }
    const auto port = audio_port_from_endpoint(endpoint_);
    std::vector<std::string> audio_args{GStreamerMediaPlatform::gst_launch_bin(), "-q"};
    const auto decode = gst_opus_rtp_decode_args(port, 100);
    audio_args.insert(audio_args.end(), decode.begin(), decode.end());

    const auto sink = choose_audio_playback_sink(false);
    bound_audio_device_ = sink.device_key.empty() ? current_audio_playback_device_key() : sink.device_key;
    audio_pipeline_info_ = sink.description;
    audio_args.insert(audio_args.end(), sink.gst_args.begin(), sink.gst_args.end());
    audio_process_.start(audio_args);
    if (wait_for_ready) {
        ensure_gst_child_stayed_up(audio_process_, "Audio", gst_video_receiver_log_path());
    }
}

void GStreamerMediaReceiver::connect(const MediaEndpoint& endpoint) {
    disconnect();
    endpoint_ = endpoint;
    video_pipeline_info_.clear();
    audio_pipeline_info_.clear();
    bound_audio_device_.clear();
    next_audio_device_check_ = std::chrono::steady_clock::now() + kAudioDevicePollInterval;

    if (!endpoint_.video_uri.empty()) {
        const auto port = video_port_from_endpoint(endpoint_);
        const auto decoder = GStreamerMediaPlatform::choose_h264_decoder();
        const auto sink = GStreamerMediaPlatform::choose_video_sink(decoder.d3d11_zero_copy);
        const auto log_path = gst_video_receiver_log_path();
        video_pipeline_info_ = std::string("decoder=") + decoder.element + " sink=" + sink.element +
            " log=" + log_path.string();

        auto environment = std::vector<std::pair<std::string, std::string>>{};
        auto unset = std::vector<std::string>{};
        GStreamerMediaPlatform::configure_display_for_sink(sink, environment, unset);

        video_process_.start(
            GStreamerMediaPlatform::standalone_video_pipeline(port, decoder, sink, false),
            environment,
            unset,
            log_path.string());
        ensure_gst_child_stayed_up(video_process_, "Video", log_path);
    }

    if (!endpoint_.audio_uri.empty()) {
        start_audio_pipeline(true);
    }
}

void GStreamerMediaReceiver::disconnect() {
    audio_process_.stop();
    video_process_.stop();
    endpoint_ = {};
    bound_audio_device_.clear();
    audio_pipeline_info_.clear();
    video_pipeline_info_.clear();
}

bool GStreamerMediaReceiver::poll() {
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
    const bool died = !audio_process_.running();
    if (!device_changed && !died) {
        return false;
    }

    audio_process_.stop();
    start_audio_pipeline(false);
    return true;
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
