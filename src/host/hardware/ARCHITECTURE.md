# src/host/hardware Architecture

`src/host/hardware/` implements physical-host adapters that do not belong to
video, audio, or peripherals.

This includes GPU enumeration, POSIX/Windows launch-environment application,
and bridging a real local controller into the virtual pad bus.

Video pipelines live in `src/host/video/`. Audio taps live in `src/host/audio/`.
`posix_launch_environment.cpp` still includes `VirtualDisplay` to set XDG for
X11 capture — that cross-link should shrink to `PresentTarget` later.
