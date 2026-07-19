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

The CLI tools use `HostSessionService` and `ClientApp` as the current reusable session layer. `ClientApp` wraps catalog sync, game filtering, controller metadata, media startup, heartbeat, and input streaming behind callbacks so a GUI can bind those events to state instead of duplicating the CLI flow.

Clients send `ViewerHeartbeat` on the TCP control channel once per second after `SessionStarting`. This applies to player clients and viewer-only clients. The host monitors those heartbeats during gameplay and stops the session if a client disconnects or exceeds `--client-timeout` seconds without a heartbeat.

## Game Metadata

The scanner looks for metadata in a parallel tree next to the ROM tree. With the default ROM root `/mnt/Internal_SSD/Gaming/ROMS/Games`, metadata defaults to `/mnt/Internal_SSD/Gaming/ROMS/Meta`.

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

Artwork is local data, not session protocol payload. Hosts and clients should use the same local asset provider against a configurable assets root. With the default ROM root `/mnt/Internal_SSD/Gaming/ROMS/Games`, assets default to `/mnt/Internal_SSD/Gaming/ROMS/Assets`.

The assets tree is based on `asset_key`, not ROM path. `asset_key` is:

```text
<system_key>/<canonical_name>/<language>/<region>/<version>
```

Images live in a directory per game:

```text
Assets/snes/super-bomberman/en/unknown/unknown/grid.png
Assets/snes/super-bomberman/en/unknown/unknown/hero.png
Assets/snes/super-bomberman/en/unknown/unknown/logo.png
Assets/snes/super-bomberman/en/unknown/unknown/icon.png
Assets/snes/super-bomberman/en/unknown/unknown/boxart.png
Assets/snes/super-bomberman/en/unknown/unknown/screenshot.png
```

The local provider also accepts common aliases such as `portrait`, `capsule`, `wide`, `background`, `cover`, and `screen`. Steam ROM Manager can populate or help choose those local images, while ArchStreamer only resolves paths from the local assets root.

`asset_probe <content-root> [metadata-root] [assets-root] [--create-dirs]` lists the expected asset directories and can create the empty directory tree.

Multiple clients can select the same `game_id` before RetroArch is launched. The host can use `SessionServer::clients_for_game()` to find which connected clients are waiting for a game and initialize RetroArch only after the intended group is ready.

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

The host CLI exposes this as `--host-role player|viewer`. Host viewer mode does not create a local input seat; it only runs the authoritative RetroArch process and will eventually capture/stream media to clients.

After a session starts, the host reopens the TCP control port for active-session joins. Late clients must select the same game and session mode as the active session. New late viewers can join with `requested_players=0`. Player clients cannot claim new seats after launch because RetroArch port assignment is fixed, but disconnected players can reconnect to their existing reserved seats.

Viewer disconnects do not stop the game session.

Player clients use a reserved-seat reconnect path. If a player control connection closes or misses heartbeats, the host marks that participant `Disconnected`, neutralizes that client's assigned virtual pads, and keeps the RetroArch seats reserved for `--player-reconnect-timeout` seconds. A reconnecting client with the same username, same game, same session mode, and same requested player count receives the original `client_id` and current seat assignment, so input resumes into the same RetroArch ports.

If the reconnect timeout expires, the host stops the session for now. This establishes the state model needed for future replacement-player handling, where the host or GUI can intentionally hand the reserved seats to a different user.

Media sender processes are owned per connected client id. Viewer disconnects stop only that viewer's audio/video senders. Player disconnects stop that player's media senders while preserving the input seats, and reconnecting players get fresh per-client media endpoints when they reclaim the same client id.

Clients can query active session state with `ActiveSessionInfoRequest`. The host returns the current game, mode, assigned player count, connected/disconnected player counts, viewer count, and media availability. Before a session starts, the same request returns `active=false`.

## Windows Client Direction

Linux remains the host target because RetroArch launching, virtual gamepads, and display/audio capture currently depend on Linux APIs. Windows clients should stay feasible by keeping client responsibilities limited to TCP/UDP protocol handling, SDL2 controller capture, local catalog cache storage, and GStreamer media receiving.

Platform-specific APIs are isolated behind compile-time platform objects. Downstream code should include the normal public headers and use `ChildProcess`, `TcpStream`, `TcpListener`, and `UdpSocket`. `common/platform/default_platform.hpp` selects the concrete implementation once. The current implementation maps those aliases to `PosixChildProcess`, `PosixTcpStream`, `PosixTcpListener`, and `PosixUdpSocket`.

Controller capture follows the same rule. Downstream code includes `client/controller_backend.hpp` and uses `ControllerBackend`. CMake selects the implementation with `ARCHSTREAMER_CONTROLLER_BACKEND`; the current supported value is `sdl2`, which aliases `ControllerBackend` to `Sdl2ControllerBackend` and links SDL2 through the `archstreamer_controller` target.

Current Windows blockers are:

- Add Windows platform objects for `ChildProcess`, `TcpStream`, `TcpListener`, and `UdpSocket`.
- Add a Windows-friendly controller backend selection and package/copy the required SDL2 runtime DLL when that backend is built for Windows.
- Embedded GUI media later will need platform-specific GStreamer window integration.

## Save Profiles

The host keeps save data under a configurable save root. The default root is `~/.local/share/archstreamer/saves`.

Each username gets its own profile directory:

```text
<save-root>/<username>/saves
<save-root>/<username>/states
```

The root also contains a `template` profile:

```text
<save-root>/template/saves
<save-root>/template/states
```

When a username is seen for the first time, the host creates that user's profile by copying the contents of `template`. RetroArch is then launched with `savefile_directory` and `savestate_directory` pointing at that user's profile.

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

When video is enabled, the host starts RetroArch on a virtual X display, captures that display with `ximagesrc`, encodes H.264 with `x264enc`, packetizes with `rtph264pay`, and sends RTP over UDP with `udpsink`. `Xvfb` is preferred for a headless display; `Xephyr` is used as the fallback when `Xvfb` is not installed.

Clients request video by default and can opt out with `session_client --no-video`. If a `MediaEndpoint` is received, the client starts a GStreamer RTP/H.264 receiver and displays it through `autovideosink`.

Session launches use per-client fanout. `--video-port` is the base UDP video port, `--audio-port` is the base UDP audio port, and each media client gets incremented ports. If `--video-dest` is omitted, the host sends media to each client's TCP peer address. If `--video-dest` is supplied, every stream uses that address with separate ports, which is useful for local multi-client testing on one machine.

Audio is opt-in on the host:

```text
host_runner --audio --audio-source <source>
```

The first audio path captures a PulseAudio/PipeWire source with `pulsesrc`, encodes Opus with `opusenc`, packetizes with `rtpopuspay`, and sends RTP over UDP. If `--audio-source` is omitted and `pactl` is available, the host uses the default sink monitor, for example `<default-sink>.monitor`, so it captures game/output audio instead of the default microphone. The client receives `rtp+opus://` endpoints with `udpsrc`, `rtpopusdepay`, `opusdec`, and `autoaudiosink`.

Current limitations:

- Fanout currently starts one video sender process and one audio sender process per media client.
- Audio and video are separate RTP streams and are not synchronized beyond normal low-latency playback buffering.
- The current receiver is a CLI process; the GUI should own the media view later.

## Input Direction

Client controller polling should produce `ControllerState`, wrap it in `ControllerInput`, serialize it, and send it to the host. The host `InputRouter` maps `(client_id, local_player)` to a RetroArch port and updates the corresponding virtual gamepad.

`session_client --input-port <port>` is the current integrated input sender. It receives the authoritative `client_id` and `SeatAssignment` from the host before sending controller packets, so it replaces the old UDP-only `input_client` probe.

Each `ControllerState` carries a monotonic `timestamp_us` captured when the client sampled the physical controller. The host tracks the last accepted timestamp per `(client_id, local_player)` and ignores older or duplicate input packets.

Client backends normalize controller input before transmission. Stick axes use signed `-32768..32767` values with a deadzone around center, so small Bluetooth/controller drift becomes zero. Triggers use unsigned `0..65535` values with a small lower deadzone.

On Linux, the virtual gamepad implementation uses `uinput`. RetroArch can then see each assigned player as a normal controller. The process needs permission to open `/dev/uinput`, which usually means a udev rule, running with elevated privileges during early testing, or adding the user to the relevant device-access group depending on the distro setup.

For local development, a udev rule like this can expose `/dev/uinput` to the `input` group:

```text
KERNEL=="uinput", GROUP="input", MODE="0660", OPTIONS+="static_node=uinput"
```

## RetroArch Direction

The host launches RetroArch with an executable path, a libretro core path, and the selected content path. The current POSIX process implementation starts RetroArch as a child process and can terminate it when the session ends.

The remaining RetroArch work is catalog construction: each `GameInfo` needs a host-side record that includes the display fields sent to clients plus the local core/content paths needed for launch.
