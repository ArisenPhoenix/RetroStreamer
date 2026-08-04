#pragma once

#include "host/launch_environment.hpp"

#include <string>

namespace archstreamer {

/**
 * One concurrent session's Pulse/PipeWire audio identity: null sink, application.id,
 * launch env, and exact-id park. Owned by ActiveSessionSlot for the session lifetime;
 * destructor unloads this slot's sink module when idle.
 *
 * Park matches application.id only (no PID-tree / binary heuristics) so concurrent
 * slots cannot steal each other's streams.
 */
class SessionAudioChannel {
public:
    explicit SessionAudioChannel(int slot_index);
    ~SessionAudioChannel();

    SessionAudioChannel(const SessionAudioChannel&) = delete;
    SessionAudioChannel& operator=(const SessionAudioChannel&) = delete;
    SessionAudioChannel(SessionAudioChannel&&) = delete;
    SessionAudioChannel& operator=(SessionAudioChannel&&) = delete;

    int slot_index() const { return slot_index_; }
    const std::string& sink_name() const { return sink_name_; }
    const std::string& application_id() const { return application_id_; }
    std::string monitor_source() const;

    /** PULSE_SINK / PULSE_PROP / PULSE_SERVER (and related) for this channel only. */
    ProcessEnvironment launch_env() const;

    /**
     * Move sink-inputs whose application.id equals this channel's id onto the
     * owned null sink. Returns how many streams were moved.
     */
    int park();

    /** Windows WASAPI: remember emulator PID for mute targeting. Linux: no-op. */
    void set_emulator_pid(int process_id);
    void clear_emulator_pid();

private:
    int slot_index_ = 0;
    std::string sink_name_;
    std::string application_id_;
    bool sink_owned_ = false;
    int emulator_pid_ = 0;
};

} // namespace archstreamer
