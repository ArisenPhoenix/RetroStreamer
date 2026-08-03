# ArchStreamer Controller Architecture

This project has three separate channels:

1. Session/control channel: reliable messages for hello, role changes, and seat assignment.
2. Input channel: low-latency controller packets from player clients to the host.
3. Media channel: GStreamer video/audio from the host to every client that requested media.

The current control prototype uses TCP-framed protocol packets. Controller input remains UDP because late input is worse than dropped input for gameplay.

Integrated control-channel smoke test:

```text
./build/host_runner --dry-run --control-port 45555 --clients 1
./build/session_client --host 127.0.0.1 --port 45555 --username test_user --mode singleplayer --players 1 --controller 0 --game 0
```

Integrated host-runner session smoke test:

```text
./build/host_runner --control-port 45555 --input-port 45454 --clients 1
./build/session_client --host 127.0.0.1 --port 45555 --input-port 45454 --username test_user --mode singleplayer --players 1 --controller 0 --game 0
```

In session mode, `host_runner` receives the selected game, requested session mode, and controller metadata from clients before launching RetroArch. It uses the first player client's username for the current RetroArch save profile. If client controller VID/PID metadata is available, `host_runner` hides those physical devices from RetroArch's SDL2 controller discovery and exposes only the ArchStreamer virtual pads.

The client declares whether it wants a `singleplayer` or `multiplayer` session. A singleplayer session can start as soon as one player is available. A multiplayer session waits until the selected game's minimum player count is available; until external metadata exists, scanned games default to `min_players=1` and `max_players=2`, and multiplayer mode requires at least two players. Direct `host_runner` launches use the same metadata checks through `--mode singleplayer|multiplayer`. If the session timeout elapses before enough players arrive, the host sends an `ErrorPacket` to connected clients and does not launch RetroArch.

The control channel now has explicit lifecycle packets:

- `SessionReady`: seats are assigned and the session has enough players.
- `SessionStarting`: host-side input routing is ready and RetroArch launch is beginning.
- `SessionEnded`: the game session ended or the host stopped it.

The CLI tools use `HostApp` / the concurrent lobby (`ActiveSessionSlot`) and `ClientApp` as the current reusable session layer. `ClientApp` wraps catalog sync, game filtering, controller metadata, media startup, heartbeat, and input streaming behind callbacks so a GUI can bind those events to state instead of duplicating the CLI flow.

Clients send `ViewerHeartbeat` on the TCP control channel once per second after `SessionStarting`. This applies to player clients and viewer-only clients. The host monitors those heartbeats during gameplay and stops the session if a client disconnects or exceeds `--client-timeout` seconds without a heartbeat.

`ViewerHeartbeat` also carries the client's wanted stream **quality** (`MediaQualityTier`: bitrate/FPS) and stream **size** (`MediaStreamSize`: 540p–1440p or Auto). Those are separate so a tall ultrawide client can request 1440p height without forcing “very high” bitrate, and vice versa. The host merges them into `VideoEncodeSettings` using the session capture resolution as the aspect-ratio source.

## Code organization

Shared helpers and session/protocol code own **what** happens and **when**. Platform translation units own **how** that step is done on this OS.

Concrete shape:

- Public headers declare a small portable API (`ChildProcess`, path helpers, discovery sockets, `raise_video_window`, session slot lease, launch environment).
- CMake selects `posix_*.cpp` or `windows_*.cpp` implementations. Downstream code includes the public header, not the platform source.
- Product/process logic stays **outside** the platform files: soft-keyboard policy, quality cutover, seat reconnect, Ryujinx profile naming, and similar belong in shared host/client modules. Platform code should still make sense if the product were renamed (`raise_window(pid)`, `kill_process_tree(pid)`, open a discovery socket).

That keeps dual-OS bugs localized (Windows Job Objects vs `PR_SET_PDEATHSIG`, X11 restack vs `SetForegroundWindow`) without forking session behavior per platform.

## Game Metadata

The scanner looks for metadata in a parallel tree next to the ROM tree. With the default ROM root `<Gaming>/ROMS/Games`, metadata defaults to `<Gaming>/ROMS/Meta`.

Metadata mirrors the ROM's relative path and replaces the ROM extension with `.json`:

```text
Games/SNES/Super Bomberman.sfc
Meta/SNES/Super Bomberman.json
```

Metadata can override the catalog display fields and declares the modes and local
player counts supported by one emulated game instance:

```json
{
  "name": "Super Bomberman",
  "system_name": "Super Nintendo Entertainment System",
  "modes": {
    "single": true,
    "multi": true
  },
  "min_players": 1,
  "max_players": 4
}
```

If metadata is missing, the scanner uses `single=true`, `multi=true`, `min_players=1`, and `max_players=2`.

## Game Selection

The host owns the available game catalog. Clients request `GameList`, render it in the GUI, then send the chosen `game_id` in `ClientHello` or `ClientConfig`.

This keeps clients from inventing paths or core settings. `game_id` is a stable host-defined identity hash, while the host keeps the local launch path privately in `HostedGame.content_path`.

Game identity is derived from canonical fields:

```text
system_key
canonical_name
version
language
region
```

The readable `identity_key` is:

```text
system=<system_key>
name=<canonical_name>
version=<version>
language=<language>
region=<region>
```

`game_id` is `sha256:<hex>` of that identity key. `language` defaults to `en`; `version` and `region` default to `unknown`. Metadata can override `system_key`, `canonical_name`, `version`, `language`, and `region`. This keeps ids stable across ROM path changes, while still allowing the client to inspect/filter the readable identity fields.

Catalog sync is revision-based. Each `GameInfo` carries an opaque host `updated_at` value, and each `GameList` carries the max `catalog_revision`. Clients cache the full catalog locally and send their cached revision in `GameListRequest.client_catalog_revision`. The host replies with:

- a full catalog when the client revision is `0`;
- a delta containing games with `updated_at` newer than the client revision;
- an empty delta when the client is already current.

The CLI client stores this at `$XDG_CACHE_HOME/archstreamer/catalog.json`, or `~/.cache/archstreamer/catalog.json` when `XDG_CACHE_HOME` is not set. Deleted game ids are represented in the protocol, but the current host scanner does not yet emit deletions from a persistent manifest.

## Game Assets

Artwork is local data, not session protocol payload. Hosts and clients should use the same local asset provider against a configurable art root. With the default ROM root `<Gaming>/ROMS/Games`, art defaults to `<Gaming>/ROMS/Art`.

The art tree is based on `asset_key`, not ROM path. `asset_key` is:

```text
<system_key>/<canonical_name>/<language>/<region>/<version>
```

Images live in a directory per game under `Art/`:

```text
Art/snes/super-bomberman/en/unknown/unknown/grid.png
Art/snes/super-bomberman/en/unknown/unknown/hero.png
Art/snes/super-bomberman/en/unknown/unknown/logo.png
Art/snes/super-bomberman/en/unknown/unknown/icon.png
Art/snes/super-bomberman/en/unknown/unknown/boxart.png
Art/snes/super-bomberman/en/unknown/unknown/screenshot.png
```

Artwork root defaults to the sibling `Art` directory next to `Games` / `Meta`
(`<Gaming>/ROMS/Art`). Until a game has local art, the GUI uses
`Art/default/default_image.png` as a placeholder. Artwork is never sent over the
session protocol; each machine resolves art locally (for example via Steam ROM
Manager on client PCs).

The local provider also accepts common aliases such as `portrait`, `capsule`, `wide`, `background`, `cover`, and `screen`. Steam ROM Manager can populate or help choose those local images, while ArchStreamer only resolves paths from the local assets root.

`asset_probe <content-root> [metadata-root] [assets-root] [--create-dirs]` lists the expected asset directories and can create the empty directory tree.

Multiple clients can select the same `game_id` before RetroArch is launched. `session_lobby` tracks connected clients in `SessionPlan` and validates that late joins match the active game and session mode.

Client-side catalog filtering is presentation-only. The host still validates the selected game, mode, and player counts authoritatively. The current client filter modes are:

- `any`: show the full catalog after optional system filtering. If no explicit session mode is selected, send `singleplayer`.
- `single`: show games with `single=true` and send `singleplayer` by default.
- `multi`: show games with `multi=true` and enough `max_players` for the requesting client, and send `multiplayer` by default.

System filtering matches the display system name and common acronyms such as `SNES`.

## Roles

Each client supplies a stable `username` in `ClientHello`. This is the host-side identity for per-user save data, save states, and future preferences. Usernames are intentionally restricted to letters, numbers, underscores, and hyphens so they can safely become part of a save directory name.

`display_name` is separate and should be treated as presentation text for the GUI. It can change without moving saves.

A client that selects zero controllers is a viewer. It can still receive video/audio, but it does not get RetroArch ports and should not send controller input.

A client that selects one or two controllers is a player client. Local player index `0` and `1` are mapped by the host to RetroArch ports. The client GUI should show this mapping after each `SeatAssignment`.

The CLI exposes this as `--role player|viewer`. Viewer role forces `requested_players=0`, skips controller capture, keeps the TCP heartbeat alive, and is intended to receive only media once the GStreamer path exists.

The host can also reserve one local player slot. When enabled, remote player assignment starts after the host port.

In session mode, a host local controller is represented as client id `0`. `host_runner --bridge-controller <index> --control-port <port> <game>` makes the host player one and routes the selected SDL2 controller through the same `InputRouter` and `SeatAssignment` path used by remote clients. For multiplayer, the host counts as one player toward the selected game's minimum player requirement. For singleplayer, the host alone can satisfy the session and launch without waiting for a remote client.

The host CLI exposes this as `--host-role player|viewer` (default `viewer`). Host viewer mode does not create a local input seat. Host player mode requires `--bridge-controller` and reserves RetroArch port 0. **Host Player** runs RetroArch on the real display with video streaming off (use Host Viewer to stream to clients on this PC or the LAN). Host Viewer enables the virtual capture display and RTP fanout when `--video` / `--audio` are on.

Video/audio streaming defaults **on** (`--video` / `--audio`; disable with `--no-video` / `--no-audio`). When video is enabled, RetroArch runs on the virtual capture display. Client `wants_video` / `wants_audio` decide which remotes get RTP fanout. The host always reserves loopback destinations at the base `--video-port` / `--audio-port` so the GUI can toggle **Watch stream locally** mid-session without changing seats (host cannot become a player mid-session).

When audio streaming is enabled without an explicit `--audio-source`, a dedicated host (Viewer) creates a Pulse/PipeWire null sink so speakers stay quiet unless **Watch stream locally** plays the RTP feed. Single-session uses `archstreamer`; concurrent session slots use `archstreamer-0`, `archstreamer-1`, … so each client hears only that slot. **Host Player** keeps the real display and default sink so singleplayer local play works; video streaming is disabled in that mode (use Host Viewer to stream to clients on this PC or the LAN).

Same-machine client+host: run Host as **Viewer** with streaming on, then on the Client tab use **This PC** (`127.0.0.1`) or LAN discovery (announcements also target loopback).

After a session starts, the host reopens the TCP control port for active-session joins. Late clients must select the same game and session mode as the active session. New late viewers can join with `requested_players=0`. Player clients cannot claim new seats after launch because RetroArch port assignment is fixed, but disconnected players can reconnect to their existing reserved seats.

Viewer disconnects do not stop the game session.

Player clients use a reserved-seat reconnect path. If a player control connection closes or misses heartbeats, the host marks that participant `Disconnected`, neutralizes that client's assigned virtual pads, and keeps the RetroArch seats reserved for `--player-reconnect-timeout` seconds. A reconnecting client with the same username, same game, same session mode, and same requested player count receives the original `client_id` and current seat assignment, so input resumes into the same RetroArch ports.

If the reconnect timeout expires, the host stops the session for now. This establishes the state model needed for future replacement-player handling, where the host or GUI can intentionally hand the reserved seats to a different user.

Media sender processes are owned per connected client id. Viewer disconnects stop only that viewer's audio/video senders. Player disconnects stop that player's media senders while preserving the input seats, and reconnecting players get fresh per-client media endpoints when they reclaim the same client id.

Clients can query active session state with `ActiveSessionInfoRequest`. The host returns the current game, mode, assigned player count, connected/disconnected player counts, viewer count, and media availability. Before a session starts, the same request returns `active=false`.

## Windows Direction

Windows is a first-class target for both **client** and **host**. The Windows client is shipped and in daily use; the Windows host path exists behind `-DARCHSTREAMER_BUILD_HOST=ON`.

### Shared platform layer

Platform-specific APIs stay behind compile-time platform objects. Downstream code includes the normal public headers and uses `ChildProcess`, `TcpStream`, `TcpListener`, and `UdpSocket`. `common/platform/default_platform.hpp` selects the concrete implementation once. On non-Windows that maps to the Posix types; on Windows it maps to Winsock2/`CreateProcess` implementations in `windows_socket` / `windows_process`. Portable cache/home/username helpers live in `common/platform/paths.hpp`, with `posix_paths.cpp` / `windows_paths.cpp` selected by CMake. Discovery sockets, process utilities, audio device enumeration, launch environment, video-window geometry, and session slot leases follow the same `posix_*` / `windows_*` split.

Controller capture follows the same rule. Downstream code includes `client/controller_backend.hpp` and uses `ControllerBackend`. CMake selects the implementation with `ARCHSTREAMER_CONTROLLER_BACKEND`; the current supported value is `sdl2`, which aliases `ControllerBackend` to `Sdl2ControllerBackend`. On Windows, ship/copy `SDL2.dll` with the binary.

Also shared across client and host on Windows:

- CMake `WIN32` source selection and dependency discovery (vcpkg / `find_package`, not Linux-only pkg-config)
- App data paths under `%LOCALAPPDATA%` (cache, art, saves) instead of XDG/`HOME` layouts
- LAN discovery via `GetAdaptersAddresses` (`windows_discovery_net.cpp`)
- GStreamer installed and on `PATH` (`gst-launch-1.0` today; in-process pipelines later)
- Child processes assigned to a Job Object with `KILL_ON_JOB_CLOSE` so orphaned `gst-launch` receivers die with the GUI

### Windows client

Client responsibilities: TCP/UDP session protocol, SDL2 controller capture, catalog/art cache, GStreamer media receive (`d3d11h264dec` / `d3d11videosink` preferred), pad on-screen keyboard, and remoted keyboard via Win32.

Client-specific notes:

- Video sinks: `d3d11videosink` with `qos=false` / `max-lateness=-1` so late-but-valid frames are not dropped under Wi‑Fi jitter
- Audio via WASAPI (`wasapi2sink` / `autoaudiosink`) when GStreamer plugins are present
- Optional Qt GUI without the Linux host stack (`ARCHSTREAMER_BUILD_HOST=OFF`)
- Windows Firewall notes for inbound RTP media ports (same class of issue as Linux)

### Windows host

Host interfaces already exist (`VirtualGamepadBus`, `RetroArchProcess`, `MediaServer` via `default_host_platform.hpp`). Linux fills them with uinput, gamescope/Xvfb/VirtualGL capture, and Pulse/PipeWire. Windows implementations:

- **Virtual pads:** ViGEmBus (`ViGEmGamepadBus`, loads `ViGEmClient.dll` at runtime)
- **Display capture:** GStreamer `d3d11screencapturesrc` (desktop) instead of gamescope PipeWire / Xvfb + `ximagesrc`
- **Audio loopback:** GStreamer `wasapisrc loopback=true` instead of Pulse/PipeWire monitors
- **Switch:** managed Yuzu/Ryujinx under `%LOCALAPPDATA%\archstreamer\`

Enable with `-DARCHSTREAMER_BUILD_HOST=ON` / `.\build_windows.ps1 -BuildHost`. Deps: `deploy/windows/install-deps.ps1`.

### Flatpak host

Flatpak GUI Host tab launches a **native** `host_runner` via `flatpak-spawn --host` (Settings → Native host_runner / `ARCHSTREAMER_HOST_RUNNER`). Gamescope/uinput/Yuzu stay on the host OS — not inside the KDE sandbox.

### Remaining follow-ups

- Embedded GUI media later needs platform-specific GStreamer window integration
- RetroArch-on-Windows host parity (paths / joypad drivers) after Switch stream proof
- Windows video-window geometry restore across cutovers (raise-to-front is implemented; full placement restore is still Linux-first)

## Save Profiles

The host keeps save data under a configurable save root (Settings → **Client save root**, or `host_runner --save-root`). The default root is `~/.local/share/archstreamer/saves`. Flatpak builds must use a path visible to the sandbox (home, or an explicit `flatpak override --filesystem=…:rw`).

Each username gets its own profile directory:

```text
<save-root>/<username>/saves
<save-root>/<username>/states
<save-root>/<username>/system   # RetroArch system_directory (BIOS / shared system files, per user)
```

Switch emulators additionally keep per-user Yuzu/Ryujinx config and NAND under the same username tree, with optional shared title saves synced across profiles where configured.

The root also contains a `template` profile:

```text
<save-root>/template/saves
<save-root>/template/states
```

When a username is seen for the first time, the host creates that user's profile by copying the contents of `template`. RetroArch is then launched with `savefile_directory`, `savestate_directory`, and `system_directory` pointing at that user's profile.

## Seat Assignment

The host owns all RetroArch port assignment. Clients only request a player count.

Current rules:

- Up to two remote clients.
- Each remote client can request zero, one, or two players.
- Zero players means viewer-only.
- The host may reserve port 0 for a local player.
- Remote clients are assigned deterministically by `client_id`.

Example with host as player and two remote clients:

| Device | Local player | RetroArch port |
| --- | --- | --- |
| Host | 0 | 0 |
| Client 1 | 0 | 1 |
| Client 1 | 1 | 2 |
| Client 2 | 0 | 3 |

## GStreamer Direction

GStreamer stays outside of the controller input path. The TCP control channel negotiates whether a client wants media, then the host sends a `MediaEndpoint` packet before `SessionStarting`.

The first implemented video path is opt-in on the host:

```text
host_runner --video --video-dest <client-ip> --video-port <udp-port>
```

When video is enabled, capture depends on the emulator:

- **Switch (Ryujinx preferred / Yuzu):** headless **gamescope** (managed wrapper under `~/.local/share/archstreamer/gamescope/` preferred). The host captures gamescope’s PipeWire Video/Source (`pipewiresrc`), not `ximagesrc`. Nested clients must load **Gamescope WSI** (`ENABLE_GAMESCOPE_WSI` + `VK_ADD_IMPLICIT_LAYER_PATH`); without it, dual-NVIDIA setups often only allow present on the boot GPU (“Device lacks a present queue” on the other card). Ryujinx is preferred for LDN/link play; per-user profiles live under the save-root username tree.
- **RetroArch:** virtual X display + `ximagesrc` by default. `Xvfb` is preferred; `Xephyr` is the fallback. When a non-default NVIDIA GPU is selected for GL, **VirtualGL** (`vglrun`) renders via the real display’s PRIME providers while capture stays on `:99`. Nintendo DS (melonDS) uses Hybrid Top layout with OpenGL renderer so R2 swaps both panes cleanly.

Encode is H.264 (`nvh264enc` when CUDA/NVENC is available, else `x264enc`), packetized with `rtph264pay`, and fanned with `multiudpsink`. Clients that share identical `VideoEncodeSettings` share an encode branch; different size/quality combinations get separate branches off a capture `tee`.

Stream size and quality are negotiated independently via `ViewerHeartbeat`:

- `MediaStreamSize` — output height ladder (`Auto`, `540p`, `720p`, `1080p`, `1440p`)
- `MediaQualityTier` — bitrate/FPS ladder (`Auto` can step up/down from host health signals)

Host capture resolution (GUI: Stream tab → Host capture resolution) sets the gamescope/Xvfb framebuffer. Scaling the stream to the client's requested height without matching capture height still letterboxes inside the encode.

Mid-session quality/size changes use a dual-stream cutover instead of tearing down the live path immediately:

1. Host sends `MediaVideoPending` with the staging RTP URI.
2. Client warms a second receiver (headless probe / staging pipeline) while the old sink keeps playing.
3. Client replies `MediaVideoReady` when the staging stream is verified.
4. Host swaps destinations, then tears down the old encode; the client switches sinks and restores window geometry when possible.

On Linux, video-window geometry (position/size/maximized/fullscreen) is captured before cutover and reapplied to the new `gst-launch` window. Closing a host-requested pad OSK also raises the video window back above the Qt GUI.

Clients request video by default and can opt out with `session_client --no-video`. Stream size/quality can be set with `--stream-size` / quality options or the GUI Stream tab. If a `MediaEndpoint` is received, the client starts a GStreamer RTP/H.264 receiver and displays it through the platform sink (`ximagesink` / `d3d11videosink` / `autovideosink`).

Session launches use per-client fanout. `--video-port` is the base UDP video port, `--audio-port` is the base UDP audio port, and each media client gets incremented ports. If `--video-dest` is omitted, the host sends media to each client's TCP peer address. If `--video-dest` is supplied, every stream uses that address with separate ports, which is useful for local multi-client testing on one machine.

Audio is opt-in on the host:

```text
host_runner --audio --audio-source <source>
```

The first audio path captures a PulseAudio/PipeWire source with `pulsesrc`, encodes Opus with `opusenc`, packetizes with `rtpopuspay`, and fans the **same** RTP packets to every destination with `multiudpsink` (Watch locally on `127.0.0.1` plus remotes). One capture/encode per session slot — not one `pulsesrc` per client. If `--audio-source` is omitted on a streaming host (Viewer), the host creates a dedicated null sink (`archstreamer` or `archstreamer-N` for concurrent slots) and captures its `.monitor` so RetroArch audio does not play on the host speakers unless **Watch stream locally** is enabled. Host Player keeps the default sink for local speakers. The client receives `rtp+opus://` endpoints with `udpsrc`, `rtpopusdepay`, `opusdec`, and `autoaudiosink` / WASAPI.

Optional synced A/V mode runs video and audio in one GStreamer process so lip-sync survives stalls better; the GUI Stream tab can also request a mid-session A/V resync.

Current limitations:

- Cutover still briefly dual-encodes; a fully seamless single-decoder switch is future work.
- Separate (non-synced) A/V streams rely on receiver buffering rather than a shared clock.
- The video sink is still an external `gst-launch` window; embedding into the Qt GUI is later.

## Input Direction

Client controller polling should produce `ControllerState`, wrap it in `ControllerInput`, serialize it, and send it to the host. The host `InputRouter` maps `(client_id, local_player)` to a RetroArch port and updates the corresponding virtual gamepad.

This path is the same for a bare-metal client and a VM client. The client uses SDL2 GameController APIs; D-pad bits may also be filled from joystick hat 0 when a device only exposes a hat (for example virtio-evdev in a guest). The host Linux uinput pad emits face/D-pad buttons and stick axes, **only on change**, and RetroArch is bound to those button indices (D-pad = 11–14). Do not also bind hat axes on the virtual pad — that double-steps menus.

`session_client --input-port <port>` is the current integrated input sender. It receives the authoritative `client_id` and `SeatAssignment` from the host before sending controller packets, so it replaces the old UDP-only `input_client` probe.

Each `ControllerState` carries a monotonic `timestamp_us` captured when the client sampled the physical controller. The host tracks the last accepted timestamp per `(client_id, local_player)` and ignores older or duplicate input packets. Remote UDP input is drained on a dedicated host thread so session heartbeats cannot stall pads.

Client backends normalize controller input before transmission. Stick axes use signed `-32768..32767` values with a deadzone around center, so small Bluetooth/controller drift becomes zero. Triggers use unsigned `0..65535` values with a small lower deadzone. Clients send a packet only when controls change (after the first sample), while still polling at a high cadence.

Face buttons use a fixed NESW letter map on the wire (`A`=South, `B`=East, `X`=West, `Y`=North). Client-side **Controller mapping** (Game Options, per system family) remaps Select/Start/L/R/L2/R2/L3/R3 and optional **Swap NW** / **Swap SE** before send. Desktop and mobile share the same portable JSON (`controller_button_map.json`, see `shared/` and `common/controller_button_map.hpp`). Stick axes are never remapped (only stick-click bits). Remapped Fast-forward uses `EmulatorControl` on the control channel. The host stays a dumb relay for face bits.

On Linux, the virtual gamepad implementation uses `uinput`. RetroArch can then see each assigned player as a normal controller. The process needs permission to open `/dev/uinput`, which usually means a udev rule, running with elevated privileges during early testing, or adding the user to the relevant device-access group depending on the distro setup.

For local development, a udev rule like this can expose `/dev/uinput` to the `input` group:

```text
KERNEL=="uinput", GROUP="input", MODE="0660", OPTIONS+="static_node=uinput"
```

### Remoted keyboard (session-wide)

Clients may also send `KeyboardInput` on the same UDP input port (Space = hold fast-forward, P = pause, F1 = menu, arrows/Enter/Esc for simple navigation). These keys are **session-wide**: the host `InputRouter` applies them without requiring a pad seat, so Viewers (0 players) can still fast-forward.

Do not conflate two different DISPLAY concepts:

- **Host capture display** (e.g. Xvfb `:99`) — where RetroArch runs and where `VirtualKeyboard` injects XTest. Private to the host.
- **Client present display** (Wayland and/or XWayland) — only where GStreamer paints the video window.

Client key capture uses `RemotedKeyboardSource`:

1. **Evdev** (`/dev/input`) — primary on Linux/Flatpak; focus-independent (works while the video window has focus). Needs `--device=all`; prefer the user in the `input` group.
2. **Gui-focus** — lobby/Qt (or other GUI) pushes held remoted keys while the ArchStreamer window has focus.
3. **X11 keymap** — last resort for pure X11 clients (SPICE VMs); not useful for native Wayland `gtksink` focus.

`KeyboardPoller` is a thin helper that owns `make_default_remoted_keyboard_source()` and stamps sequence/timestamps for the wire. Flatpak clients should use `--socket=wayland` plus `--socket=x11` (XWayland available) and `--device=all`; see `deploy/flatpak/README.md`.

### Soft keyboard (pad OSK)

Some emulators (notably Ryujinx Avalonia swkbd) open a real text dialog on the capture display that cannot be typed with a gamepad alone. The host watches for a focused soft-keyboard dialog, publishes `SoftKeyboardRequest` on the control channel, and the client GUI opens a pad-driven on-screen keyboard. `SoftKeyboardResponse` carries the accepted text (or cancel); the host XTest-types it into the dialog (clearing any seeded nickname first).

The watcher is session-scoped and re-arms after each dialog closes, so declining an in-game “is this name correct?” confirmation still prompts again. Closing a host-requested OSK raises the video sink window back to the foreground so the Qt GUI does not leave the game buried.

## Concurrent sessions

A persistent host lobby can run multiple singleplayer session slots. Each slot claims machine-wide exclusive resources through `SessionSlotLease` (display numbers, audio null-sink names, base media ports, uinput product-id offsets) so two simultaneous games do not steal each other's capture display or save/system paths. Multiplayer still uses one shared-emulator lobby.

## GUI layout

The Qt GUI tabs are organized by concern:

- **Client** — connect/join, game picker, controllers, role/mode
- **Host** — ports, lobby mode, stream toggles, advertise
- **Stream** — client quality/size and A/V receive options; host capture resolution, GPU, renderer scales
- **Game Options** — per-family controller remaps + face-button swaps, pad OSK test button, per-game helpers
- **Profile** — usernames / Steam account
- **Settings** — art/ROM/meta roots, audio device refresh, lobby wait, log level

Host and client last-selected games are persisted independently.

## RetroArch Direction

The host launches RetroArch with an executable path, a libretro core path, and the selected content path. The current POSIX process implementation starts RetroArch as a child process and can terminate it when the session ends.

The remaining RetroArch work is catalog construction: each `GameInfo` needs a host-side record that includes the display fields sent to clients plus the local core/content paths needed for launch.
