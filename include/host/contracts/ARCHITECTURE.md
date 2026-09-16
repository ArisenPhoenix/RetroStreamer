# include/host/contracts Architecture

`include/host/contracts/` is the shared language between host concerns.

It holds data types only — no GStreamer, no uinput, no RetroArch. Video, audio,
peripherals (`virtual/`), and emulators (`console/`) speak these structs across
the session plug:

- `present.hpp`: `PresentTarget` — video produces the surface; emulators consume
  display/env; session starts capture from it.
- `audio.hpp`: `AudioTap` — audio owns the sink/loopback; emulators and capture
  both read the source name.
- `seat.hpp`: `PadPlan` — peripherals resolve pads after plug; emulators bind
  them. This is how virtual inputs enter an emulator.
- `launch.hpp`: `EmulatorPlug` / `EmulatorLaunchEnvRequest` — session fills
  `request.plug` from present + audio + seat; `build_emulator_launch_environment`
  reads the plug. Emulator-only bits (GPU, profiles, session id) stay on the
  request.
- `media.hpp`: combined capture request for the session-facing `MediaServer`.
  Fill it with `make_media_capture_config(present, audio_tap, …)`.

Implementations live in `video/`, `audio/`, `virtual/`, `console/`, and
`hardware/` (OS env application). Session orchestrates; it should not invent
parallel flag bags.

Host-facing knobs (`VideoSettings`, `AudioSettings`) live in the owning module
and are embedded by `HostAppConfig`. Those are configure-time; `PresentTarget` /
`AudioTap` / `PadPlan` are the runtime plug language.
