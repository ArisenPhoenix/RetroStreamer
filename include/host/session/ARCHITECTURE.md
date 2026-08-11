# include/host/session Architecture

`include/host/session/` defines active gameplay session architecture.

This bucket owns:

- session launch plans and assembled launch state;
- active slot runtime and cleanup;
- backend preparation and emulator lifecycle interfaces;
- control monitoring and heartbeat handling;
- session-scoped audio/control helpers;
- reusable session state types.

It should receive validated lobby output and prepared host configuration, then
own the running session until teardown.
