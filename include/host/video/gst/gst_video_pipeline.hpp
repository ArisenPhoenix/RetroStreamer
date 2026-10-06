#pragma once

#include "host/video/gst/gst_video_command.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace archstreamer {

struct GstVideoH264Options {
    bool use_nvenc = false;
    bool managed_nvenc_device_select = false;
    int nvenc_cuda_device_id = -1;
};

/** In-process factory lookup. Cached; does not spawn gst-inspect-1.0. */
bool gst_video_element_available(const char* element);
bool gst_video_element_has_property(const char* element, const char* property);

/** gst_parse_launch quoting: double quotes only. */
std::string gst_video_parse_quote(std::string_view value);

GstVideoH264Options gst_video_probe_h264(int nvenc_cuda_device_id);

std::vector<std::pair<std::string, std::string>> gst_video_source_selectors(
    const GstVideoSource& source);

std::string gst_video_build_description(
    const GstVideoGraph& graph,
    std::string_view selector_property,
    std::string_view selector_value,
    const GstVideoH264Options& options);

std::string gst_video_encode_csv(const GstVideoEncode& encode);
bool gst_video_decode_csv(std::string_view text, GstVideoEncode& encode);

std::string gst_video_encode_graph(const GstVideoGraph& graph);
bool gst_video_decode_graph(std::string_view text, GstVideoGraph& graph);

std::string gst_video_graph_summary(const GstVideoGraph& graph);

/** `$XDG_RUNTIME_DIR/archstreamer/video/<id>.session` (or `/tmp` when unset). */
std::filesystem::path gst_video_pipeline_record_path(std::string_view pipeline_id);

/** First line `id=<pipeline name>` from a Ready or Status event. */
std::string gst_video_event_pipeline_id(std::string_view message);

} // namespace archstreamer
