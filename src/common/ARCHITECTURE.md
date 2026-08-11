# src/common Architecture

`src/common/` implements the shared data and protocol layer.

This code backs packet serialization, discovery, normalized controls, catalog
identity, pairing, hashes, assets, and portable helpers. It is the lowest common
runtime layer used by host, client, GUI, and tools.

It should not know about active host sessions, Qt widgets, or emulator process
details.
