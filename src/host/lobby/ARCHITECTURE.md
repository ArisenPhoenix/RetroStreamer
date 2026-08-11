# src/host/lobby Architecture

`src/host/lobby/` implements pre-session coordination.

The lobby gathers clients, validates selected game/mode/player counts, handles
join/rejoin requests, and builds the handoff into active session launch.

It should not own emulator processes after launch; that responsibility belongs
to `src/host/session/`.
