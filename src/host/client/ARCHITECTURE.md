# src/host/client Architecture

`src/host/client/` implements host-side client policy.

This code interprets client capability and heartbeat data into stream policy and
adaptation decisions. It should stay independent of UI presentation and emulator
process launch details.
