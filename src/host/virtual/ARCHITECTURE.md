# src/host/virtual Architecture

`src/host/virtual/` implements emulator-visible peripherals.

This includes virtual gamepads, keyboard injection, network input receivers,
seat planning, device-plan resolution (`device_plan.cpp`), and input routing.
It converts client/controller intent into device state the emulator consumes
via `PadPlan`. VID/PID ignore-list strings (`hex_vid_pid`) are formatted here.

Linux uinput pads are opened by the session process (`EVIOCGRAB`) and inherited
by whatever emulator that process spawns. Firejail LDN (`--keep-fd=all` plus
`--env=LD_PRELOAD=libarchstreamer_pad_access.so`) has to keep those fds; it
otherwise closes them and strips `LD_PRELOAD`. Desktop GLFW (Prism/Minecraft)
can still open the node via systemd-uaccess, but the grab means it receives no
events. `deploy/udev/99-archstreamer-virtual-pads.rules` additionally mode-000s
the node when installed.

Present surfaces (Xvfb/gamescope) moved to `src/host/video/`.
