# include/host/video

Present-surface and video-capture headers live here.

Virtual displays, `VideoSettings`, `MediaConfig` (embedded in `HostAppConfig`),
`MediaPlanConfig` / `MediaDestination` (`MediaServer.start()` args),
present resolution (`CaptureResolveInput` → `PresentTarget`),
`SessionCaptureDevices`, and media senders belong in this directory.
`gst/` is the GStreamer encode command/engine API (no `gst.h` in those headers).
Shared types (`PresentTarget`, `GpuTarget`) live under `include/host/contracts/`.
Emulator-visible pads and keyboards belong under `include/host/virtual/`.
