# include/host/virtual Architecture

`include/host/virtual/` defines emulator-visible peripherals.

This bucket owns virtual gamepads, virtual keyboards, seat resolution,
network input receiving, and input routing into emulator devices.

The seat *language* (`PadPlan`) lives in `host/contracts/seat.hpp`. This
directory resolves and applies it. `PadSettings` is the host-facing knobs
`HostAppConfig` embeds. Device-plan helpers take `PadSettings` rather than
`HostAppConfig`. VID/PID ignore-list formatting (`hex_vid_pid`) lives here;
cadence stores the integer product base. Present surfaces and
`SessionCaptureDevices` live in `host/video/`. Physical host devices belong in
`host/hardware/`.
