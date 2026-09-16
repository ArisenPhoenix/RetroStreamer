# src/host/hardware Architecture

`src/host/hardware/` implements physical-host adapters that do not belong to
video, audio, or peripherals.

This includes GPU enumeration, POSIX/Windows launch-environment application,
and bridging a real local controller into the virtual pad bus.

Video pipelines live in `src/host/video/`. Audio taps live in `src/host/audio/`.
`capture_launch_environment` takes `PresentTarget`. POSIX still calls
`gamescope_launch_environment` / `virtual_gl_environment` from `VirtualDisplay`.
