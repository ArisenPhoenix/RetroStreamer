# src/host/virtual Architecture

`src/host/virtual/` implements emulator-visible peripherals.

This includes virtual gamepads, keyboard injection, network input receivers,
seat planning, device-plan resolution (`device_plan.cpp`), and input routing.
It converts client/controller intent into device state the emulator consumes
via `PadPlan`. VID/PID ignore-list strings (`hex_vid_pid`) are formatted here.

Present surfaces (Xvfb/gamescope) moved to `src/host/video/`.
