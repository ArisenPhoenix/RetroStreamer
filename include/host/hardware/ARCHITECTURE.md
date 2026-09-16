# include/host/hardware Architecture

`include/host/hardware/` defines leftover physical-host adapters.

This bucket now owns GPU selection, `GpuSettings` (embedded in `HostAppConfig`),
`SessionGpuSelection` / `resolve_session_gpu_selection` (takes `GpuSettings`,
emits `GpuTarget`), applying `EmulatorLaunchEnvRequest` to a child environment,
local controller bridging, and the OS composition header
(`default_host_platform.hpp`).

Video capture belongs in `host/video/`. Audio taps belong in `host/audio/`.
The shared plug language belongs in `host/contracts/`. Virtual pads belong in
`host/virtual/`. Console backends fill `request.profile_env`; session fills
`request.gpu_env` from `render_gpu_environment` on the resolved `GpuTarget`.
