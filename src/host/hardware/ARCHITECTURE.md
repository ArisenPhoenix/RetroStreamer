# src/host/hardware Architecture

`src/host/hardware/` implements physical-host adapters that do not belong to
video, audio, or peripherals.

This includes GPU enumeration, session GPU selection
(`gpu_selection.cpp` produces `GpuTarget` from `GpuDevice`), POSIX/Windows
launch-environment application, and bridging a real local controller into the
virtual pad bus.

Video pipelines live in `src/host/video/`. Audio taps live in `src/host/audio/`.
`capture_launch_environment` takes `PresentTarget`. POSIX still calls
`gamescope_launch_environment` / `virtual_gl_environment` from `VirtualDisplay`.
`build_emulator_launch_environment` merges `request.gpu_env` and
`request.profile_env` — it does not include console profile types.
