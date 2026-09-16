# src/host/console Architecture

`src/host/console/` implements emulator, catalog, and content behavior.

This includes catalog scanning/repair, RetroArch helpers, standalone emulator
resolution, save-profile handling, console-specific services, and leftover
emulator process reaping.

Session lifecycle code should call into this layer for content and emulator
facts, then keep runtime ownership in `src/host/session/`.

`PosixRetroArchProcess::stop()` SIGTERMs the guest emulator (Ryujinx, Yuzu,
RetroArch, …) while gamescope/Xwayland is still up, then compositors, then
firejail. SIGKILL is last. That avoids Ryujinx aborting on a dead `:20` and
leaves Switch saves a few seconds to flush. After the tree is gone it drops
stale gamescope `/tmp/.Xn-lock` reservation files.
