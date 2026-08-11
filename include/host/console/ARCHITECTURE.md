# include/host/console Architecture

`include/host/console/` defines emulator, catalog, and content boundaries.

This bucket owns:

- game catalog scanning, repair, and metadata;
- RetroArch process/config/netcmd helpers;
- standalone emulator descriptions;
- save-profile resolution and console-specific save handling;
- system-specific subdirectories such as `switch/` and `nds/`.

It should not own session admission, active connection state, or physical capture
devices. Those belong in `lobby/`, `session/`, and `hardware/`.
