# src/host/virtual Architecture

`src/host/virtual/` implements emulator-visible virtual devices.

This includes virtual gamepads, keyboard injection, virtual display helpers,
network input receivers, seat planning, and input routing.

It converts client/controller intent into device state the emulator can consume.

Xvfb/Xephyr startup waits until the Unix socket accepts a connection, not merely
until `/tmp/.X11-unix/Xn` exists, so `ximagesrc` does not race keymap compile.
