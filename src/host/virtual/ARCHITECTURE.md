# src/host/virtual Architecture

`src/host/virtual/` implements emulator-visible peripherals.

This includes virtual gamepads, keyboard injection, network input receivers,
seat planning, and input routing. It converts client/controller intent into
device state the emulator consumes via `PadPlan`.

Present surfaces (Xvfb/gamescope) moved to `src/host/video/`.
