# include/host Architecture

`include/host/` defines the host's subsystem boundaries.

The root headers are for top-level orchestration or cross-cutting services:
`HostApp`, launch planning, save management, pairing relay, link coordination,
and small diagnostics. Most new host behavior should land in a child directory
that names the concern it owns.

The main buckets are:

- `contracts/`: shared language (`PresentTarget`, `GpuTarget`, `AudioTap`, `PadPlan`, `EmulatorPlug`).
- `client/`: host-side client metadata and stream adaptation.
- `console/`: emulator/content/save-profile configuration.
- `db/`: database-backed persistence APIs.
- `video/`: present surfaces and outbound video.
- `audio/`: streaming audio taps.
- `hardware/`: leftover physical adapters (GPU, launch-env application, local pads).
- `lobby/`: pre-session gathering and join/rejoin admission.
- `session/`: active session plans, runtime state, and lifecycle helpers.
- `user/`: user identity and user-facing sync behavior.
- `virtual/`: emulator-visible peripherals and input routing.

The host flow should pass `host/contracts/` types between these areas rather than
rebuilding loose primitive groups in each layer. Host-facing knobs
(`MediaConfig`, `PadSettings`, `GpuSettings`, `GameConfig`, `EmulatorConfig`)
live in the owning module and are embedded by `HostAppConfig`. `MediaConfig`
composes `VideoSettings` + `AudioSettings`; MediaServer resolves Off /
VideoOnly / AudioOnly / Both from it.
