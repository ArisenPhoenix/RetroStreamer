# src/host/db Architecture

`src/host/db/` implements host persistence stores.

This code owns schema-backed reads/writes for catalog metadata, active sessions,
Cadence tracking, and user control mappings. Higher layers should use these
stores instead of embedding SQL or persistence details directly.
