#pragma once

#include <string>
#include <string_view>

namespace archstreamer {

// Linux: Pulse/PipeWire-Pulse null sink(s) so game audio stays off the real
// speakers unless Watch-local (or a remote) plays the RTP feed.
// Windows: WASAPI session mute for tracked emulator processes (same call sites);
// system loopback capture continues via WindowsMediaServer.
//
// Single-session host uses sink name "archstreamer".
// Concurrent session slots own a SessionAudioChannel (archstreamer-N + exact
// application.id park); StreamingAudioSink remains for legacy single-session
// and lobby prune/restore helpers.
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

    /**
     * Drop leftover ArchStreamer null sinks from older runs.
     * keep_legacy: retain the single-session "archstreamer" sink.
     * max_slots: keep archstreamer-0 .. archstreamer-(max_slots-1) only.
     */
    void prune_unused(int max_slots, bool keep_legacy);

    // Move Viewer RetroArch sink-inputs onto the legacy null sink.
    void park_game_audio();
    // Move only this slot's tagged streams onto archstreamer-N.
    void park_game_audio_for_slot(int slot_index);

    // If the session default was left on a null sink, point it at a real device.
    void restore_default_sink();

    // Default Pulse sink's .monitor (no ArchStreamer null sink).
    static std::string default_monitor_source();

    /**
     * Remember the emulator PID (and optional concurrent slot) so park can
     * target that process tree. Windows: WASAPI mute. Linux: Pulse move onto
     * the slot null sink (needed when firejail PID namespaces make
     * application.process.id useless and PULSE_PROP is stripped/ignored).
     * slot_index < 0 → single-session / legacy park_game_audio().
     */
    void track_emulator_process(int process_id, int slot_index = -1);
    void untrack_emulator_process(int slot_index = -1);
};

} // namespace archstreamer
