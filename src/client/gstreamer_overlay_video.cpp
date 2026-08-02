#include "client/gstreamer_overlay_video.hpp"

#include <atomic>
#include <mutex>
#include <sstream>
#include <string>

#if defined(ARCHSTREAMER_HAS_GST_LIBS)
#include <gst/app/gstappsink.h>
#include <gst/gst.h>
#include <gst/video/video.h>
#endif

namespace archstreamer {
namespace {

#if defined(ARCHSTREAMER_HAS_GST_LIBS)
std::once_flag g_gst_once;
std::atomic<bool> g_gst_ok{false};

void ensure_gst() {
    std::call_once(g_gst_once, [] {
        GError* error = nullptr;
        if (gst_init_check(nullptr, nullptr, &error)) {
            g_gst_ok.store(true);
        }
        if (error != nullptr) {
            g_error_free(error);
        }
    });
}

bool has_element_factory(const char* name) {
    auto* factory = gst_element_factory_find(name);
    if (factory == nullptr) {
        return false;
    }
    gst_object_unref(factory);
    return true;
}

std::string build_pipeline_desc(std::uint16_t port) {
    std::ostringstream desc;
    desc
        << "udpsrc port=" << port
        << " buffer-size=" << (4 * 1024 * 1024)
        << " caps=\"application/x-rtp,media=video,encoding-name=H264,payload=96,clock-rate=90000\""
        << " ! rtpjitterbuffer latency=50 drop-on-latency=true do-lost=true"
        << " ! rtph264depay ! ";
    if (has_element_factory("h264parse")) {
        desc << "h264parse ! ";
    }
    if (has_element_factory("avdec_h264")) {
        desc << "avdec_h264 output-corrupt=false ! ";
    } else if (has_element_factory("openh264dec")) {
        desc << "openh264dec ! ";
    } else {
        return {};
    }
    desc
        << "videoconvert ! video/x-raw,format=RGBA ! "
        << "appsink name=videosink emit-signals=true sync=false max-buffers=2 drop=true";
    return desc.str();
}

GstFlowReturn on_new_sample(GstAppSink* appsink, gpointer user_data) {
    auto* self = static_cast<GStreamerOverlayVideo*>(user_data);
    GstSample* sample = gst_app_sink_pull_sample(appsink);
    if (sample == nullptr) {
        return GST_FLOW_OK;
    }
    GstBuffer* buffer = gst_sample_get_buffer(sample);
    GstCaps* caps = gst_sample_get_caps(sample);
    if (buffer == nullptr || caps == nullptr) {
        gst_sample_unref(sample);
        return GST_FLOW_OK;
    }

    GstVideoInfo info;
    if (!gst_video_info_from_caps(&info, caps)) {
        gst_sample_unref(sample);
        return GST_FLOW_OK;
    }

    GstVideoFrame frame;
    if (!gst_video_frame_map(&frame, &info, buffer, GST_MAP_READ)) {
        gst_sample_unref(sample);
        return GST_FLOW_OK;
    }
    const int w = GST_VIDEO_INFO_WIDTH(&info);
    const int h = GST_VIDEO_INFO_HEIGHT(&info);
    const int stride = GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0);
    const auto* data = static_cast<const std::uint8_t*>(GST_VIDEO_FRAME_PLANE_DATA(&frame, 0));
    self->publish_sample(data, w, h, stride);
    gst_video_frame_unmap(&frame);
    gst_sample_unref(sample);
    return GST_FLOW_OK;
}

void clear_appsink_callbacks(GstElement* sink) {
    // gst_app_sink_set_callbacks asserts callbacks != NULL — pass an empty struct.
    GstAppSinkCallbacks empty{};
    gst_app_sink_set_callbacks(GST_APP_SINK(sink), &empty, nullptr, nullptr);
}
#endif

} // namespace

bool gstreamer_overlay_video_available() {
#if defined(ARCHSTREAMER_HAS_GST_LIBS)
    ensure_gst();
    if (!g_gst_ok.load()) {
        return false;
    }
    return has_element_factory("appsink") &&
        (has_element_factory("avdec_h264") || has_element_factory("openh264dec"));
#else
    return false;
#endif
}

GStreamerOverlayVideo::GStreamerOverlayVideo() = default;

GStreamerOverlayVideo::~GStreamerOverlayVideo() {
    stop();
}

void GStreamerOverlayVideo::set_frame_bridge(std::shared_ptr<VideoEmbedBridge> bridge) {
    std::lock_guard lock(mutex_);
    frame_bridge_ = std::move(bridge);
}

void GStreamerOverlayVideo::publish_sample(
    const std::uint8_t* data,
    int width,
    int height,
    int stride) {
    std::shared_ptr<VideoEmbedBridge> bridge;
    {
        std::lock_guard lock(mutex_);
        ++frames_seen_;
        bridge = frame_bridge_;
    }
    if (bridge) {
        bridge->publish_frame(data, width, height, stride);
    }
}

std::uint64_t GStreamerOverlayVideo::frames_seen() const {
    std::lock_guard lock(mutex_);
    return frames_seen_;
}

bool GStreamerOverlayVideo::start(std::uint16_t udp_port, std::uint64_t /*window_handle*/) {
    stop();
    std::lock_guard lock(mutex_);
    port_ = udp_port;
    frames_seen_ = 0;
    return build_and_play(udp_port);
}

bool GStreamerOverlayVideo::switch_port(std::uint16_t udp_port) {
    stop();
    std::lock_guard lock(mutex_);
    port_ = udp_port;
    frames_seen_ = 0;
    return build_and_play(udp_port);
}

void GStreamerOverlayVideo::stop() {
    std::lock_guard lock(mutex_);
    teardown(true);
}

void GStreamerOverlayVideo::request_stop() {
    std::lock_guard lock(mutex_);
    teardown(false);
}

bool GStreamerOverlayVideo::running() const {
#if defined(ARCHSTREAMER_HAS_GST_LIBS)
    std::lock_guard lock(mutex_);
    if (pipeline_ == nullptr) {
        return false;
    }
    auto* pipeline = static_cast<GstElement*>(pipeline_);
    GstState state = GST_STATE_NULL;
    gst_element_get_state(pipeline, &state, nullptr, 0);
    return state == GST_STATE_PLAYING || state == GST_STATE_PAUSED;
#else
    return false;
#endif
}

void GStreamerOverlayVideo::set_render_size(int, int) {}

void GStreamerOverlayVideo::expose() {}

bool GStreamerOverlayVideo::build_and_play(std::uint16_t udp_port) {
#if defined(ARCHSTREAMER_HAS_GST_LIBS)
    ensure_gst();
    if (!g_gst_ok.load()) {
        return false;
    }
    const auto desc = build_pipeline_desc(udp_port);
    if (desc.empty()) {
        pipeline_info_ = "no H.264 decoder for appsink path";
        return false;
    }

    GError* error = nullptr;
    auto* pipeline = gst_parse_launch(desc.c_str(), &error);
    if (error != nullptr) {
        pipeline_info_ = std::string("parse error: ") + error->message;
        g_error_free(error);
        if (pipeline != nullptr) {
            gst_object_unref(pipeline);
        }
        return false;
    }
    if (pipeline == nullptr) {
        pipeline_info_ = "parse_launch returned null";
        return false;
    }

    auto* sink = gst_bin_get_by_name(GST_BIN(pipeline), "videosink");
    if (sink == nullptr) {
        pipeline_info_ = "appsink missing";
        gst_object_unref(pipeline);
        return false;
    }

    GstAppSinkCallbacks callbacks{};
    callbacks.new_sample = on_new_sample;
    gst_app_sink_set_callbacks(GST_APP_SINK(sink), &callbacks, this, nullptr);

    auto* bus = gst_element_get_bus(pipeline);
    const auto ret = gst_element_set_state(pipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        pipeline_info_ = "set_state PLAYING failed appsink";
        gst_object_unref(bus);
        gst_object_unref(sink);
        gst_object_unref(pipeline);
        return false;
    }

    GstState state = GST_STATE_NULL;
    gst_element_get_state(pipeline, &state, nullptr, 400 * GST_MSECOND);
    if (state != GST_STATE_PLAYING && state != GST_STATE_PAUSED) {
        pipeline_info_ = "appsink pipeline left early";
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_element_get_state(pipeline, nullptr, nullptr, 2 * GST_SECOND);
        gst_object_unref(bus);
        gst_object_unref(sink);
        gst_object_unref(pipeline);
        return false;
    }

    pipeline_ = pipeline;
    bus_ = bus;
    appsink_ = sink;
    pipeline_info_ =
        std::string("overlay appsink RGBA port=") + std::to_string(udp_port);
    return true;
#else
    (void)udp_port;
    pipeline_info_ = "ARCHSTREAMER_HAS_GST_LIBS not enabled";
    return false;
#endif
}

void GStreamerOverlayVideo::teardown(bool wait_for_null) {
#if defined(ARCHSTREAMER_HAS_GST_LIBS)
    if (pipeline_ == nullptr) {
        port_ = 0;
        return;
    }
    auto* pipeline = static_cast<GstElement*>(pipeline_);
    auto* bus = static_cast<GstBus*>(bus_);
    auto* sink = static_cast<GstElement*>(appsink_);

    if (sink != nullptr) {
        clear_appsink_callbacks(sink);
    }

    gst_element_set_state(pipeline, GST_STATE_NULL);
    if (wait_for_null) {
        // Do not block — even 200ms decoder drains hitch the desktop on Ctrl+C.
        gst_element_get_state(pipeline, nullptr, nullptr, 0);
        if (bus != nullptr) {
            gst_object_unref(bus);
            bus_ = nullptr;
        }
        if (sink != nullptr) {
            gst_object_unref(sink);
            appsink_ = nullptr;
        }
        gst_object_unref(pipeline);
        pipeline_ = nullptr;
    }
    // request_stop: leave pointers so a later stop() can wait + free.
#else
    (void)wait_for_null;
#endif
    port_ = 0;
}

} // namespace archstreamer
