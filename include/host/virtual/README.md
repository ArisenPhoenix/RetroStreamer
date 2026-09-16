# include/host/virtual

Virtual peripheral headers live here.

Gamepads, keyboards, input routing, seat *resolution*, and `PadSettings`
(embedded in `HostAppConfig`) belong in this directory. `PadPlan` itself is
the shared seat language under `include/host/contracts/seat.hpp`. SDL
VID/PID formatting (`hex_vid_pid`) lives here. Present surfaces belong
under `include/host/video/`.
