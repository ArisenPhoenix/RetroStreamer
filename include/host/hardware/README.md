# include/host/hardware

Physical host adapter headers live here.

GPU selection, launch-environment *application* (POSIX/Windows env), and local
controller bridging belong in this directory. Present surfaces and media
senders moved to `include/host/video/`. Audio taps moved to
`include/host/audio/`. Shared types live under `include/host/contracts/`.
