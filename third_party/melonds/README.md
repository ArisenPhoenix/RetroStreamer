# ArchStreamer melonDS patch

Standalone NDS uses a patched melonDS binary with headless LAN controls so Link
can arm Local Wireless mid-session without opening the melonDS UI.

## Install layout

| Item | Path |
|------|------|
| Binary | `/srv/emus/melonDS` (also `ARCHSTREAMER_MELONDS`) |
| Managed copy | `~/.local/share/archstreamer/melonds/melonDS` |
| BIOS / firmware | `~/.local/share/archstreamer/system/nds/{bios7,bios9,firmware}.bin` |
| Per-user config | `<save-root>/<user>/melonds/xdg-config/melonDS/melonDS.toml` |

## CLI / control socket (this patch)

```
--lan-host
--lan-connect <host>
--lan-player <name>
--lan-players <count>
--archstreamer-ctrl <name>   # QLocalServer; Linux socket /tmp/<name>
```

Control lines (newline-terminated): `LAN_HOST <player> [n]`, `LAN_CONNECT <player> <host>`,
`LAN_END`, `TOUCH <x> <y>`, `TOUCH_END`, `SCREENS`, `PAUSE on|off|toggle`, `PAUSE` (status),
`PING`
→ replies `OK` / `ERR …` / `PONG` / `OK 0|1` for pause status.

`PAUSE on|off` calls melonDS `emuPause`/`emuUnpause` (absolute). Do not use keyboard F5 for
ArchStreamer drawer/menu pause — F5 is a toggle and desyncs absolute On/Off.

`SCREENS` → `OK <ww> <wh> <hasTop> <tx> <ty> <tw> <th> <hasBot> <bx> <by> <bw> <bh>`
(window + top/bottom AABBs in panel pixels; follows swap/emphasis).

Also apply [screens-aabb-matrix.patch](screens-aabb-matrix.patch) so `queryScreenRects`
matches melonDS `M23_Transform` (otherwise bottom AABB is wrong and clients
mis-detect emphasis / miss stylus hits).

## Rebuild local tree

```bash
cd ~/Games/melonDS   # or your melonDS checkout
git apply /path/to/ArchStreamer/third_party/melonds/archstreamer-lan.patch
git apply /path/to/ArchStreamer/third_party/melonds/screens-aabb-matrix.patch
# or copy ArchStreamerCtrl.{h,cpp} and merge CLI/main/CMake changes
cmake --build build -j"$(nproc)"
cp -v build/melonDS /srv/emus/melonDS
cp -v build/melonDS ~/.local/share/archstreamer/melonds/melonDS
```

ArchStreamer host code talks only through `MelonDsBackend` (`include/host/nds/`).
