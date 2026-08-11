# include/host/db Architecture

`include/host/db/` defines persistence boundaries for host-owned state.

This bucket owns database stores for catalog metadata, active sessions, Cadence
tracking, and user control mappings. It should expose narrow store APIs and avoid
driving session flow directly.

Feature code may interpret persisted values, but the DB layer should remain the
place where schema-backed storage is created, read, migrated, and updated.
