# include Architecture

`include/` is the public C++ boundary for the native codebase.

Consumers should include headers from here by stable project paths, for example
`host/session/runtime.hpp` or `common/protocol.hpp`. The implementation lives
under `src/` and should mirror these boundaries when the code is not
platform-specific.

The architectural split is:

- `client/`: desktop client contracts.
- `common/`: shared protocol, normalized data, and portable helpers.
- `host/`: host subsystem contracts.
- `tools/`: CLI and probe app wrappers.

Headers should describe ownership and behavior without depending on executable
entry points.
