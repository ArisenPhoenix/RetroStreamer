# include/host/video Architecture

`include/host/video/` owns present surfaces and outbound video.

This bucket covers virtual displays (Xvfb, Xephyr, VirtualGL, gamescope),
capture-plan resolution, the `MediaServer` video API, and GStreamer/DXGI
senders.

Session should take a `PresentTarget` from `host/contracts/present.hpp` rather
than Switch-vs-PS2 flag bags. Emulators consume the same target's display/env.

`capture_platform.hpp` takes `CaptureResolveInput` (from `VideoSettings` plus
emulator facts) and returns a `PresentTarget`.
`resolve_present_target_from_settings` writes any software-core cap back into
`VideoSettings`. `SessionCaptureDevices` is the runtime capture bind (present +
`GpuTarget` + resolution). Session translates `HostAppConfig.media.video` + RetroArch
into that input and speaks the target for keyboard, warmup, GPU, and media start.

Not yet cohesive:

- `MediaServer` is the session-facing A/V facade (intentional). Audio
  encode lives in `host/audio/`; this object composes it from `MediaConfig`.
- `HostAppConfig` embeds `MediaConfig` (video + audio + host loopback);
  MediaServer resolves the send profile. Sync is a client playback choice.
