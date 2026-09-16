# include/host

Host-side public headers live here.

The root of this directory contains top-level host orchestration APIs and small
cross-cutting host services. Concern-specific code should live in a child
directory:

- `contracts/`: shared language between video, audio, peripherals, and emulators.
- `client/`: host-side view of connected clients and stream policy.
- `console/`: console, emulator, content, and save-profile handling.
- `db/`: persistence stores and database-backed state.
- `video/`: present surfaces and outbound video.
- `audio/`: streaming audio taps.
- `hardware/`: leftover physical adapters (GPU, launch-env application).
- `lobby/`: pre-session lobby and session hub coordination.
- `platform/`: narrow platform abstraction headers that do not fit elsewhere.
- `session/`: active session lifecycle, launch plans, and runtime state.
- `user/`: user identity and user-facing sync helpers.
- `virtual/`: virtual peripherals and input routing.
