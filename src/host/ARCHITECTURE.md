# src/host Architecture

`src/host/` implements the host runtime.

The root implementation coordinates top-level flow. Concern-specific work belongs
in child directories:

- `client/`: client stream policy and adaptation.
- `console/`: game catalog, emulator process/config, and save-profile work.
- `db/`: persistence stores.
- `video/`: present surfaces and video capture/encode.
- `audio/`: streaming audio taps.
- `hardware/`: GPU, launch-env application, local controller bridge.
- `lobby/`: client gathering and join/rejoin admission.
- `session/`: active session lifecycle and runtime ownership.
- `user/`: user credentials and sync helpers.
- `virtual/`: virtual peripherals and input routing.

New code should prefer passing `host/contracts/` types across these layers instead of
reconstructing ad hoc groups of primitive values.
