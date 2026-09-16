# include/host/virtual Architecture

`include/host/virtual/` defines emulator-visible peripherals.

This bucket owns virtual gamepads, virtual keyboards, seat resolution,
network input receiving, and input routing into emulator devices.

The seat *language* (`PadPlan`) lives in `host/contracts/seat.hpp`. This
directory resolves and applies it. Present surfaces moved to `host/video/`.
Physical host devices belong in `host/hardware/`.
