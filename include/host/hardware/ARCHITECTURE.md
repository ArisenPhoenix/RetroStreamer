# include/host/hardware Architecture

`include/host/hardware/` defines physical host platform contracts.

This bucket owns display/audio capture, media sender abstractions, GPU selection,
launch environment discovery, local controller bridges, and OS-specific media
services.

Virtual emulator-facing devices belong in `host/virtual/`. Console-specific
emulator configuration belongs in `host/console/`.
