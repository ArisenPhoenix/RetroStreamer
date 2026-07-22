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

## Notes / limits

- **Client use** (join a host, controllers, video/audio receive) is the main Flatpak target.
- **Host use** on Bazzite usually needs a **native** (or distrobox) build of ArchStreamer so it can
  launch Flatpak RetroArch (`org.libretro.RetroArch`) and use `/dev/uinput`. Host-inside-Flatpak
  is still limited.
- Grant filesystem access to your ROM/Art paths if they live outside `$HOME`
  (the manifest already allows `home` and `/mnt:ro`).
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
