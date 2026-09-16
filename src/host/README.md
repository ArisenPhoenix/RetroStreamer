# src/host

Host implementation lives here.

The root contains top-level orchestration and cross-cutting host services.
Concern-specific implementation should live in child directories:

- `client/`: connected-client stream policy and adaptation.
- `console/`: emulator, catalog, and save-profile handling.
- `db/`: persistence implementations.
- `video/`: present surfaces and video pipelines.
- `audio/`: streaming audio taps.
- `hardware/`: GPU, launch-env application, local controller bridge.
- `lobby/`: lobby/session hub coordination.
- `session/`: active session lifecycle and runtime state.
- `user/`: user identity and sync behavior.
- `virtual/`: virtual peripherals and routing.
