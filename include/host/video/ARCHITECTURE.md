# include/host/video Architecture

`include/host/video/` owns present surfaces and outbound video.

This bucket covers virtual displays (Xvfb, Xephyr, VirtualGL, gamescope),
capture-plan resolution, the `MediaServer` video API, and GStreamer/DXGI
senders.

Session should take a `PresentTarget` from `host/contracts/present.hpp` rather
than Switch-vs-PS2 flag bags. Emulators consume the same target's display/env.

`capture_platform.hpp` takes `CaptureResolveInput` (from `VideoSettings` plus
emulator facts) and returns a `PresentTarget`. Session translates
`HostAppConfig.video` + RetroArch into that input and speaks the target
for keyboard, warmup, GPU, and media start.

Not yet cohesive:

- `MediaServer` is the session-facing A/V facade (intentional). Audio
  encode lives in `host/audio/`; this object composes it.
- `HostAppConfig` now embeds `VideoSettings` / `AudioSettings`; session
  should keep consuming those rather than growing a parallel bag.
