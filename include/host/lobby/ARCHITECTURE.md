# include/host/lobby Architecture

`include/host/lobby/` defines pre-session coordination.

The lobby receives client hellos/configuration, validates join intent, tracks
when enough players are present, handles join/rejoin admission, and hands a
coherent launch request to active session code.

Once the emulator is launched and seats are active, ownership moves to
`host/session/`.
