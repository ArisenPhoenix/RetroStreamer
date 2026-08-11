# src/host/console Architecture

`src/host/console/` implements emulator, catalog, and content behavior.

This includes catalog scanning/repair, RetroArch helpers, standalone emulator
resolution, save-profile handling, and console-specific services.

Session lifecycle code should call into this layer for content and emulator
facts, then keep runtime ownership in `src/host/session/`.
