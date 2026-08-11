# ArchStreamer

ArchStreamer hosts emulators on a PC and streams gameplay to desktop and Android
clients while routing remote controls back to the host.

## Directory Map

- [branding](branding/README.md): application icons, names, and packaged branding assets.
- [deploy](deploy/README.md): install, update, packaging, and environment setup scripts.
- [docs](docs/README.md): design notes and behavior documentation.
- [include](include/README.md): public C++ headers.
- [mobile](mobile/README.md): Android client project and mobile-specific integration.
- [runtime_cadence](runtime_cadence/README.md): sidecar runtime/state service.
- [scripts](scripts/README.md): developer and host setup utilities.
- [shared](shared/README.md): data files consumed by more than one target.
- [src](src/README.md): C++ implementation files.
- [third_party](third_party/README.md): vendored or embedded third-party code.

Generated directories such as `build/`, `_dependencies/`, `.cache/`, and
`.flatpak-builder/` are not source layout roots.

## Architecture

Start with [ARCHITECTURE.md](ARCHITECTURE.md). It explains how the directories
compose into the running host, desktop client, Android client, tools, and shared
runtime pieces, then links to the directory-specific architecture notes.

## Current Limitations

The source layout is now bucketed by concern, but the architecture is still in a
transitional state. Some host concepts, especially `HostApp`, session runtime,
backend prep, lobby handoff, media lifecycle, and emulator-specific launch
behavior, still need stronger interfaces.

Backend abstraction is not fully formalized yet. RetroArch, Ryujinx/Yuzu, and
melonDS share common phases, but prepare, launch, control, pause, teardown, save
handling, touch, and link behavior are not all behind one stable backend
contract.

Android still mirrors some native protocol/data definitions in Kotlin. That is
functional, but it can drift from the C++ protocol layer unless changes are kept
in sync carefully.

Media adaptation is practical rather than fully principled. TV-specific stream
guardrails and feedback exist, but the system still needs clearer telemetry,
documented thresholds, and a stronger client-pressure model before aggressive
self-tuning is safe.

Video cutover remains one of the riskiest areas. Reconnect, stale RTP, SPS/PPS
timing, keyframe availability, and decoder reset behavior need targeted tests.

Platform support is uneven. Linux host is the strongest path, Windows client is
usable, Windows host exists but is more conditional, and Android/TV behavior
still needs device-targeted testing.

Persistence and identity are early. User IDs, host IDs, case-sensitive usernames,
and password flow have improved, but password storage and authentication need a
more explicit security model.

Automated testing is thin around the highest-risk behavior: join/rejoin, stream
cutover, QR sync, control mappings, session ownership, and media recovery.

melonDS integration depends on manually applying patch files to an external
checkout. A pinned fork or scripted fetch/apply/build flow would make that path
more reproducible.

The documentation is new and should be kept honest as cleanup continues. When
ownership boundaries become real interfaces, the README and architecture files
should be updated with the code.
