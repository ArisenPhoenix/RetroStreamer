# ArchStreamer Controller Architecture

This project has three separate channels:

1. Session/control channel: reliable messages for hello, role changes, and seat assignment.
2. Input channel: low-latency controller packets from player clients to the host.
3. Media channel: GStreamer video/audio from the host to every client that requested media.

## Roles

A client that selects zero controllers is a viewer. It can still receive video/audio, but it does not get RetroArch ports and should not send controller input.

A client that selects one or two controllers is a player client. Local player index `0` and `1` are mapped by the host to RetroArch ports. The client GUI should show this mapping after each `SeatAssignment`.

The host can also reserve one local player slot. When enabled, remote player assignment starts after the host port.

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

Keep GStreamer outside of the controller protocol. The control channel should send or negotiate the media endpoint, then `MediaServer` and `MediaReceiver` can use whatever pipeline is appropriate for the platform.

A practical first version is:

- Host capture: RetroArch window or pipewire/ximagesrc.
- Encode: hardware H.264/H.265 where available.
- Transport: RTP over UDP for LAN, WebRTC later if NAT traversal is needed.
- Audio: Opus over RTP.

## Input Direction

Client controller polling should produce `ControllerState`, wrap it in `ControllerInput`, serialize it, and send it to the host. The host `InputRouter` maps `(client_id, local_player)` to a RetroArch port and updates the corresponding virtual gamepad.

On Linux, the virtual gamepad implementation will likely use `uinput`. RetroArch can then see each assigned player as a normal controller.
