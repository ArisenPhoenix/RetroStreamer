# runtime_cadence Architecture

`runtime_cadence/` implements the Cadence sidecar.

The sidecar owns runtime state storage and session/user operations that are kept
outside the main GUI and host process. Native host code should treat it as a
separate service boundary rather than mixing sidecar persistence directly into
session orchestration.
