# src/tools Architecture

`src/tools/` implements command-line executables and probes.

Tools should remain thin adapters over reusable host, client, and common
libraries. If a behavior is needed by both GUI and CLI, it should live outside
`src/tools/`.
