# Flatpak (Bazzite / immutable Linux)

Bazzite and other atomic desktops often lack compile-time packages on the host.
A Flatpak is the easiest way to run the ArchStreamer GUI there.

## Build a bundle (on a machine with `flatpak-builder`)

```bash
./scripts/build-flatpak.sh
```

This produces `build-flatpak/ArchStreamer.flatpak`.

## Install on Bazzite

Copy the `.flatpak` over, then:

```bash
flatpak install --user ./ArchStreamer.flatpak
flatpak run io.github.ArisenPhoenix.ArchStreamer
```

## Client vs Host

### Client (in Flatpak)

Bundled: Qt 6 (KDE Platform 6.9), SDL2, nlohmann_json, GStreamer from the runtime.
Join a LAN host, controllers, video/audio receive — no extra packages.

### Host (Flatpak GUI → native `host_runner`)

The Flatpak includes the Host tab and may ship a sandboxed `host_runner`, but **Switch / gamescope / uinput sessions must run on the host OS**.

When started inside Flatpak, Host uses:

```text
flatpak-spawn --host <native-host_runner> …
```

Configure the native binary in **Settings → Native host_runner**, or set `ARCHSTREAMER_HOST_RUNNER`.

Build that binary outside the sandbox (native or distrobox) with the usual Linux host deps:

| Need | Why |
|---|---|
| RetroArch | Non-Switch cores |
| `/dev/uinput` | Virtual pads |
| GStreamer (+ PipeWire / NVENC as needed) | Capture + encode — `scripts/install_gst.sh` |
| **gamescope** + Gamescope WSI | Switch / Yuzu headless |
| Yuzu AppImage + keys | Switch titles |
| Optional VirtualGL + Xvfb | RetroArch multi-GPU GL |

Manifest finish-args already include `--talk-name=org.freedesktop.Flatpak` for spawn.

### Filesystem notes

- Grant ROM/Art paths outside `$HOME` if needed (manifest allows `home` and `/mnt:ro`).
- Steam art import: Flatpak Steam userdata under `~/.var/app/com.valvesoftware.Steam/`.

## Updates / reinstall

No Flathub remote yet. Rebuild and:

```bash
flatpak install --user --reinstall ./build-flatpak/ArchStreamer.flatpak
```
