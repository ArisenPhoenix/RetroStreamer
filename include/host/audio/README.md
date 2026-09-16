# include/host/audio

Streaming audio-tap headers live here.

Null-sink / loopback helpers, per-slot `SessionAudioChannel`, `AudioSettings`
(embedded in `HostAppConfig`), and the opus RTP fanout belong in this directory.
The shared `AudioTap` language lives under `include/host/contracts/`. Session
talks to `MediaServer` (in `include/host/video/`) which composes this fanout
with video.
