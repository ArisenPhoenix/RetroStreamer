# src/host/virtual Architecture

`src/host/virtual/` implements emulator-visible virtual devices.

This includes virtual gamepads, keyboard injection, virtual display helpers,
network input receivers, seat planning, and input routing.

It converts client/controller intent into device state the emulator can consume.
