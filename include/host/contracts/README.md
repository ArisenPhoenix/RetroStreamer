# include/host/contracts

Shared host language lives here.

These headers are the types video, audio, peripherals, and emulators pass
through session. They must stay free of backend implementations.

- `present.hpp`: present surface (`PresentTarget`).
- `gpu.hpp`: resolved GPU bind (`GpuTarget`).
- `audio.hpp`: capture tap (`AudioTap`).
- `seat.hpp`: virtual-pad contract (`PadPlan`).
- `launch.hpp`: combined emulator plug and process environment. GPU/profile
  layers are `ProcessEnvironment` bags, not emulator types.
- `media.hpp`: legacy combined capture config (video+audio still mixed).
