# src/host

Host implementation lives here.

The root contains top-level orchestration and cross-cutting host services.
Concern-specific implementation should live in child directories:

- `client/`: connected-client stream policy and adaptation.
- `console/`: emulator, catalog, and save-profile handling.
- `db/`: persistence implementations.
- `hardware/`: physical host hardware and OS media/platform adapters.
- `lobby/`: lobby/session hub coordination.
- `session/`: active session lifecycle and runtime state.
- `user/`: user identity and sync behavior.
- `virtual/`: virtual input/display devices and routing.
