#pragma once

#include <string>

namespace archstreamer {

// Owns the Pulse/PipeWire-Pulse null sink used while streaming so game audio stays
// off the real speakers unless Watch-local (or a remote) plays the RTP feed.
class StreamingAudioSink {
public:
    static constexpr const char* kName = "archstreamer";

    // Create/resume the null sink; returns its pactl name.
    std::string ensure();
    // ensure() + ".monitor" for pulsesrc capture.
    std::string monitor_source();

    // Move Viewer RetroArch sink-inputs onto the null sink (defeats stream-restore leaks).
    void park_game_audio();
    // If the session default was left on the null sink, point it at a real device.
    void restore_default_sink();

    // Default Pulse sink's .monitor (no ArchStreamer null sink).
    static std::string default_monitor_source();
};

} // namespace archstreamer
