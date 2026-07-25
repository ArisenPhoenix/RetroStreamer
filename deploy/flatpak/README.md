# Flatpak (Bazzite / immutable Linux)

Bazzite and other atomic desktops often lack compile-time packages on the host.
A Flatpak is the easiest way to run the ArchStreamer **client** GUI there.

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

## Dependencies

### Bundled in the Flatpak (no extra install for client)

| Piece | Source |
|---|---|
| Qt 6 Widgets | `org.kde.Platform` 6.9 |
| GStreamer (decode / video window) | KDE Platform |
| SDL2 | Manifest module |
| nlohmann_json | Manifest module |

Recent host features (**Gamescope WSI**, VirtualGL, Yuzu AppImage, dual-GPU / NVENC, RetroArch `.opt` resolution) do **not** add Flatpak modules. They are Linux **host** runtime tools and are not required to join a session as a client.

### Host on Bazzite / immutable (native or distrobox — not the Flatpak)

Run a native/`host_runner` build for Host Viewer / Host Player. Install on the host OS (or distrobox):

| Need | Why |
|---|---|
| RetroArch (`org.libretro.RetroArch` or system) | Non-Switch cores |
| `/dev/uinput` access | Virtual pads |
| GStreamer tools + plugins (incl. PipeWire / NVENC if used) | Capture + encode — see `scripts/install_gst.sh` |
| **gamescope** (managed copy under `~/.local/share/archstreamer/gamescope/` preferred) | Switch / Yuzu headless capture |
| Gamescope WSI layer (`ENABLE_GAMESCOPE_WSI`, layer next to managed gamescope) | Dual-NVIDIA: non-boot GPU can present into nested XWayland |
| Yuzu AppImage + keys under `~/.local/share/archstreamer/yuzu/` | Switch titles |
| Optional: VirtualGL (`vglrun`) + Xvfb | RetroArch multi-GPU GL when not using gamescope |
| Optional: NVIDIA driver / NVENC | Hardware encode; PRIME providers for OpenGL offload |

Host-inside-Flatpak remains limited (sandbox cannot cleanly own gamescope/WSI, uinput, and host RetroArch). Prefer native/distrobox for hosting.

### Filesystem notes

- Grant ROM/Art paths outside `$HOME` if needed (manifest already allows `home` and `/mnt:ro`).
- Steam art import needs Steam userdata readable; Flatpak Steam uses
  `~/.var/app/com.valvesoftware.Steam/` (under home).

## Updates / reinstall

There is **no Flathub remote** for this app yet. `flatpak update` will not pull newer ArchStreamer
builds by itself.

- Same machine you build on: rebuild, then
  `flatpak install --user --reinstall ./build-flatpak/ArchStreamer.flatpak`
- Other machine: copy the new `.flatpak` over and install/reinstall that file.
  Reinstalling an **old** bundle just puts the old version back.

## Rebuild tips

```bash
# after code changes
./scripts/build-flatpak.sh
flatpak install --user --reinstall ./build-flatpak/ArchStreamer.flatpak
```
