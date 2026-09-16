# include/host/console Architecture

`include/host/console/` defines emulator, catalog, and content boundaries.

This bucket owns:

- game catalog scanning, repair, and metadata (`GameConfig`);
- emulator IR knobs (`EmulatorConfig` / `ResolutionSettings`);
- RetroArch process/config/netcmd helpers (`RetroArchOverrideParams` lives
  with the config writer);
- content/assets resolution (`launch_content.hpp`);
- standalone emulator descriptions;
- save-profile resolution and console-specific save handling;
- system-specific subdirectories such as `switch/` and `nds/`.

It should not own session admission, active connection state, or present/audio
capture. Those belong in `lobby/`, `session/`, `video/`, and `audio/`.
Emulator process reaping lives here (`emulator_orphan_reaper.hpp`).
Profile types stay in this bucket; backends merge `yuzu_launch_environment` /
`ryujinx_launch_environment` / `melonds_launch_environment` into
`EmulatorLaunchEnvRequest.profile_env` rather than putting those structs on
the contracts request.
