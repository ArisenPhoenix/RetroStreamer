# include/host/video

Present-surface and video-capture headers live here.

Virtual displays, `VideoSettings` (embedded in `HostAppConfig`), present
resolution (`CaptureResolveInput` → `PresentTarget`), and media senders belong
in this directory.
Shared types (`PresentTarget`) live under `include/host/contracts/`.
Emulator-visible pads and keyboards belong under `include/host/virtual/`.
