# include/host/client

Host-side client descriptors live here.

This directory is for the host's model of a connected client: identity,
capabilities, stream policy, and stream adaptation signals.

It should not contain desktop-client implementation code; that belongs under
`include/client/` and `src/client/`.
