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

Remoted keyboard (Space = fast-forward, etc.) uses the `RemotedKeyboardSource`
stack: **evdev** (`/dev/input`) is primary and focus-independent; the Qt lobby
feeds **gui-focus** when the ArchStreamer window is focused; X11 keymap is a
last resort for pure X11 VMs. Flatpak needs `--device=all` and `--socket=x11`
(alongside Wayland). Add the user to the `input` group, then log out and back in:

```bash
# Bazzite/ostree: ensure the group exists in /etc/group first
grep -q '^input:' /etc/group || sudo bash -c 'grep ^input: /usr/lib/group >> /etc/group'
sudo usermod -aG input "$USER"
# then log out and back in
```

Flatpak already uses `--device=all`; without the group only gamepad nodes are
readable, and the X11 keymap fallback does not see Wayland video-window focus.

### Larger UDP receive buffers (optional, Wi‑Fi)

Flatpak cannot grant real `CAP_NET_ADMIN`. GStreamer’s `udpsrc buffer-size=…`
needs `SO_RCVBUF` above `net.core.rmem_max`, which fails with
“Need net.admin privilege?” and drops large H.264 keyframe bursts on Wi‑Fi.

On the **client** machine, a one-time sudo script raises kernel `rmem_*` limits
and sets Flatpak env `ARCHSTREAMER_UDP_RCVBUF` so ArchStreamer can request a
larger buffer safely:

```bash
# from a checkout of this repo, or copy the script alone
sudo ./scripts/grant-flatpak-udp-buffers.sh
# then restart the Flatpak
flatpak run io.github.ArisenPhoenix.ArchStreamer

# undo later:
sudo ./scripts/grant-flatpak-udp-buffers.sh --revoke
```

Defaults are ~8 MiB `rmem_max` and a 2 MiB app request. Override with
`ARCHSTREAMER_RMEM_MAX` / `ARCHSTREAMER_UDP_RCVBUF` when invoking the script.
Without this script the client still works (kernel-default buffers).

Always install the app with `--user` (not system-wide). A system install asks for
a password on every update and can shadow the user copy:

```bash
flatpak install --user ./ArchStreamer.flatpak
# if you previously installed system-wide:
# flatpak uninstall --system io.github.ArisenPhoenix.ArchStreamer
```

### Host (Flatpak GUI → native `host_runner`)

The Flatpak includes the Host tab and may ship a sandboxed `host_runner`, but **Switch / gamescope / uinput sessions must run on the host OS**.

When started inside Flatpak, Host uses:

```text
flatpak-spawn --host <native-host_runner> …
```

Configure the native binary in **Settings → Native host_runner**, or set `ARCHSTREAMER_HOST_RUNNER`.

On Bazzite (immutable), build it in a Fedora distrobox and export to the host:

```bash
distrobox create --name archstreamer-build --image fedora:43 \
  --volume /var/srv:/var/srv:rw --volume /srv:/srv:ro
# install cmake/SDL2/GStreamer/X11 devel packages inside the box, then:
# also: pulseaudio-utils (pactl) so StreamingAudioSink can create the null sink
# against the host PipeWire socket shared via /run/user/$UID
cmake -S . -B build-native -G Ninja -DARCHSTREAMER_BUILD_HOST=ON -DARCHSTREAMER_REQUIRE_GUI=OFF
cmake --build build-native --target host_runner
distrobox-export --bin "$PWD/build-native/host_runner" --export-path ~/.local/bin
```

Then point Settings at `~/.local/bin/host_runner` (or rely on auto-detect / `which host_runner`).

Distrobox packages typically needed: `cmake ninja-build gcc-c++ SDL2-devel json-devel gstreamer1-devel gstreamer1-plugins-base-devel pipewire-devel pulseaudio-libs-devel pulseaudio-utils libX11-devel libXtst-devel`.

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

- Manifest allows `home`, `/mnt:ro`, `/var/srv:ro`, and `/srv:ro`.
- If ROMs live elsewhere, grant access without rebuilding:

```bash
flatpak override --user --filesystem=/path/to/catalog:ro io.github.ArisenPhoenix.ArchStreamer
```

- Steam art import: Flatpak Steam userdata under `~/.var/app/com.valvesoftware.Steam/`.

## Updates / reinstall

No Flathub remote yet. Rebuild and:

```bash
flatpak install --user --reinstall ./build-flatpak/ArchStreamer.flatpak
```
