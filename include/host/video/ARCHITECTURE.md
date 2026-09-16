# include/host/video Architecture

`include/host/video/` owns present surfaces and outbound video.

This bucket covers virtual displays (Xvfb, Xephyr, VirtualGL, gamescope),
capture-plan resolution, the `MediaServer` video API, and GStreamer/DXGI
senders.

Session should take a `PresentTarget` from `host/contracts/present.hpp` rather
than Switch-vs-PS2 flag bags. Emulators consume the same target's display/env.

Not yet cohesive:

- `MediaServer` still starts audio fanout; that belongs in `host/audio/`.
- `capture_platform.hpp` still includes `HostAppConfig` and RetroArch launch
  types — video should not need the emulator catalog to pick Xvfb vs gamescope.
