# src/client Architecture

`src/client/` implements the desktop client runtime.

The runtime connects to the host, syncs catalog/art data, captures input,
forwards controls, starts media playback, and reports heartbeats/stream
preferences.

Platform-specific desktop adapters live here when they are client-only. Shared
protocol logic remains in `src/common/`.
