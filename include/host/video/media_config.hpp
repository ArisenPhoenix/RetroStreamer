#pragma once

#include "host/audio/audio_settings.hpp"
#include "host/contracts/media.hpp"
#include "host/video/video_settings.hpp"

namespace archstreamer {

/**
 * Resolved send profile. MediaServer picks this from which tracks are enabled.
 * Client playback chooses Independent vs Synced on the received URIs.
 */
enum class MediaProfile {
    Off,
    VideoOnly,
    AudioOnly,
    Both,
};

/**
 * Host-facing media knobs. MediaServer owns this composition so session does
 * not thread video and audio as parallel flag bags. VideoSettings / AudioSettings
 * stay the per-track types; host_local_watch is the combined loopback option.
 *
 * Sync is a client playback choice (`ClientAppConfig.synced_av`), not a host knob.
 */
struct MediaConfig {
    VideoSettings video;
    AudioSettings audio;
    bool host_local_watch = false;

    [[nodiscard]] bool enabled() const { return video.enabled || audio.enabled; }
    [[nodiscard]] AudioTap tap() const { return audio.tap(); }
};

inline MediaProfile resolve_media_profile(const MediaConfig& media) {
    if (media.video.enabled && media.audio.enabled) {
        return MediaProfile::Both;
    }
    if (media.video.enabled) {
        return MediaProfile::VideoOnly;
    }
    if (media.audio.enabled) {
        return MediaProfile::AudioOnly;
    }
    return MediaProfile::Off;
}

/**
 * Capture request MediaServer implementations consume. Session passes MediaConfig
 * plus the resolved present surface; this fills GStreamerMediaCaptureConfig.
 */
inline GStreamerMediaCaptureConfig resolve_media_capture(
    const MediaConfig& media,
    const PresentTarget& present,
    bool verbose,
    int nvenc_cuda_device_id) {
    const auto profile = resolve_media_profile(media);
    return make_media_capture_config(
        profile == MediaProfile::VideoOnly || profile == MediaProfile::Both,
        profile == MediaProfile::AudioOnly || profile == MediaProfile::Both,
        present,
        media.tap(),
        media.video.resolution,
        verbose,
        nvenc_cuda_device_id);
}

} // namespace archstreamer
