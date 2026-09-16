# src/host/audio Architecture

`src/host/audio/` implements Pulse/PipeWire/WASAPI streaming taps.

It creates the per-host (or per-slot) capture source that emulators play into
and that the media path records. `SessionAudioChannel` is implemented here
(`streaming_audio_sink.cpp`). `GStreamerAudioFanout` is the opus RTP runner
composed by `GStreamerMediaServer` (the session-facing A/V facade).
