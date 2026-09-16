# include/host/session

Active session headers live here.

This directory owns session plans, launch assembly, active slot runtime, backend
prep, session lifecycle, and reusable session types. `launch_types.hpp` composes
owned module types; `backend_state.hpp` holds Switch/melonDS session backends.
Per-slot audio taps live under `include/host/audio/`.

Lobby admission and pre-session waiting belong under `include/host/lobby/`.
