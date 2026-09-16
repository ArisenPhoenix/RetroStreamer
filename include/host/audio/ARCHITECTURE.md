# include/host/audio Architecture

`include/host/audio/` owns streaming audio capture taps.

This bucket covers null-sink / WASAPI loopback helpers that produce an
`AudioTap` (`host/contracts/audio.hpp`), plus the opus RTP fanout
(`gstreamer_audio_fanout.hpp`) composed by `MediaServer`. Emulators inherit
the tap as `PULSE_SINK` (or equivalent); the fanout captures the monitor.

`SessionAudioChannel` is the per-slot tap (lifetime owned by the active
session). `AudioSettings` is the host-facing knobs `HostAppConfig` embeds.
`normalize_audio_backend_for_platform` lives here too.
