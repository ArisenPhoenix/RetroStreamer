# src/host Architecture

`src/host/` implements the host runtime.

The root implementation coordinates top-level flow. Concern-specific work belongs
in child directories:

- `client/`: client stream policy and adaptation.
- `console/`: game catalog, emulator process/config, and save-profile work.
- `db/`: persistence stores.
- `hardware/`: capture, audio, GPU, launch environment, and OS media adapters.
- `lobby/`: client gathering and join/rejoin admission.
- `session/`: active session lifecycle and runtime ownership.
- `user/`: user credentials and sync helpers.
- `virtual/`: virtual devices and input routing.

New code should prefer passing structured buckets across these layers instead of
reconstructing ad hoc groups of primitive values.
