# include/host Architecture

`include/host/` defines the host's subsystem boundaries.

The root headers are for top-level orchestration or cross-cutting services:
`HostApp`, launch planning, save management, pairing relay, link coordination,
and small diagnostics. Most new host behavior should land in a child directory
that names the concern it owns.

The main buckets are:

- `client/`: host-side client metadata and stream adaptation.
- `console/`: emulator/content/save-profile configuration.
- `db/`: database-backed persistence APIs.
- `hardware/`: physical host platform, capture, audio, GPU, and media adapters.
- `lobby/`: pre-session gathering and join/rejoin admission.
- `session/`: active session plans, runtime state, and lifecycle helpers.
- `user/`: user identity and user-facing sync behavior.
- `virtual/`: emulator-visible virtual devices and input routing.

The host flow should pass structured buckets between these areas rather than
rebuilding loose primitive groups in each layer.
