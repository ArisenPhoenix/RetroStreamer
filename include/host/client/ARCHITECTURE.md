# include/host/client Architecture

`include/host/client/` defines how the host describes connected clients.

This bucket owns client identity, device capability hints, requested stream
quality/size, and adaptation signals reported by clients. It does not implement
desktop or Android UI behavior.

Session, lobby, and media code should receive this information as a client
bucket instead of passing unrelated primitive fields.
