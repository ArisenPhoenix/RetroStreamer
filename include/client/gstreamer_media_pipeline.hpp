#pragma once

#include "client/gstreamer_probe.hpp"
#include "common/platform/default_platform.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace archstreamer {

// Shared gst-launch fragments — no platform branching.

std::filesystem::path gst_video_receiver_log_path();
std::filesystem::path gst_synced_receiver_log_path();

void ensure_gst_child_stayed_up(
    const ChildProcess& process,
    const char* label,
    const std::filesystem::path& log_path);

// RTP H.264 receive through depay (no decoder / sink yet).
std::vector<std::string> gst_h264_rtp_source_args(std::uint16_t port);

// Opus RTP receive through resample (no sink yet).
std::vector<std::string> gst_opus_rtp_decode_args(std::uint16_t port, int jitter_latency_ms);

void gst_append_h264parse_if_available(std::vector<std::string>& args);

// Software path: videoconvert ! progressreport ! <sink> sync=<sync>
void gst_append_progress_video_sink(
    std::vector<std::string>& args,
    const GstVideoSinkChoice& sink,
    bool sync);

} // namespace archstreamer
