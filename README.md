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
