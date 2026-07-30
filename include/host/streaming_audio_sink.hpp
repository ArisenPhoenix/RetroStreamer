#pragma once

#include <string>
#include <string_view>

namespace archstreamer {

// Owns Pulse/PipeWire-Pulse null sink(s) used while streaming so game audio stays
// off the real speakers unless Watch-local (or a remote) plays the RTP feed.
//
// Single-session host uses sink name "archstreamer".
// Concurrent session slots use "archstreamer-0", "archstreamer-1", … so each
// GStreamer capture hears only that slot's emulator.
class StreamingAudioSink {
public:
    static constexpr const char* kName = "archstreamer";

    /** Sink name for concurrent slot N (`archstreamer-0`, …). */
    static std::string slot_sink_name(int slot_index);
    /** Pulse application.id set on the emulator so park can target one slot. */
    static std::string slot_application_id(int slot_index);
    /** True if name is our capture sink (legacy or slotted). */
    static bool is_streaming_sink_name(std::string_view sink_name);

    // Create/resume the legacy null sink; returns its pactl name.
    std::string ensure();
    // ensure() + ".monitor" for pulsesrc capture.
    std::string monitor_source();

    // Per-slot null sink + monitor (concurrent sessions).
    std::string ensure_slot(int slot_index);
    std::string monitor_source_for_slot(int slot_index);

    // Move Viewer RetroArch sink-inputs onto the legacy null sink.
    void park_game_audio();
    // Move only this slot's tagged streams onto archstreamer-N.
    void park_game_audio_for_slot(int slot_index);

    // If the session default was left on a null sink, point it at a real device.
    void restore_default_sink();

    // Default Pulse sink's .monitor (no ArchStreamer null sink).
    static std::string default_monitor_source();
};

} // namespace archstreamer
