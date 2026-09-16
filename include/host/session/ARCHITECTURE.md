# include/host/session Architecture

`include/host/session/` defines active gameplay session architecture.

This bucket owns:

- session launch plans and assembled launch state;
- active slot runtime and cleanup;
- backend preparation and emulator lifecycle interfaces;
- control monitoring and heartbeat handling;
- reusable session state types.

It should receive validated lobby output and prepared host configuration, then
own the running session until teardown. Cross-concern data should be
`PresentTarget`, `GpuTarget`, `AudioTap`, `PadPlan`, and `EmulatorPlug` from
`host/contracts/`.

`launch_types.hpp` is the session compose header (launch environment, media
plan, participants, plug bind). Console backends live in `backend_state.hpp`
so including launch types does not pull Switch/melonDS. GPU selection,
device plans, and content/assets live in `hardware/`, `virtual/`, and
`console/` respectively; those APIs take module settings (`GpuSettings`,
`PadSettings`, `GameConfig` / `EmulatorConfig`) rather than `HostAppConfig`.
