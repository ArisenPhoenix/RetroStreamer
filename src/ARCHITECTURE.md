# src Architecture

`src/` contains C++ implementation files.

It mirrors `include/` where possible:

- `client/`: desktop client runtime implementation.
- `common/`: shared protocol and utility implementation.
- `gui/`: Qt GUI composition.
- `host/`: host runtime implementation.
- `tools/`: executable entry points and probes.

Implementation files should preserve the ownership boundaries described by the
matching headers.
