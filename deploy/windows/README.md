# Windows client and host

**Python front-ends** (preferred; leave the `.ps1` files as references):

```powershell
python build_windows.py
python deploy\update_and_install.py --reset-hard
python deploy\install_deps.py
python deploy\finish_install.py --shortcuts
```

## Roles

| Mode | CMake | What you get |
|---|---|---|
| **Client** (default) | `-DARCHSTREAMER_BUILD_HOST=OFF` | Join LAN sessions, controllers, GStreamer receive |
| **Host** | `-DARCHSTREAMER_BUILD_HOST=ON` | Also `host_runner` — Yuzu + ViGEm pads + DXGI/WASAPI capture |

Linux-only tools (gamescope, Gamescope WSI, VirtualGL, uinput) are **not** used on Windows.

## Update / install (client PC)

Repo usually lives at `%USERPROFILE%\Documents\RetroStreamer`.

```powershell
# After Linux pushes to GitHub — pull deploy branch (master), build, install:
.\deploy\windows\update-and-install.ps1 -ResetHard

# Test another branch in isolation (e.g. refactor work on dev):
.\deploy\windows\update-and-install.ps1 -ResetHard -Branch dev

# Build/install only (no git):
.\deploy\windows\update-and-install.ps1 -SkipPull
```

`-Branch` defaults to **`master`** (deploy / stable). Pass another name to pull that ref instead.

`-ResetHard` discards local edits on the Windows tree so it always matches `origin/<Branch>` (recommended for a pure client machine).

Install overwrites `C:\Program Files\ArchStreamer\bin\*.exe`. Run an **Admin** PowerShell, and close `archstreamer_gui` / `session_client` first (the script stops them when it can). A locked `session_client.exe` usually shows as cmake `file INSTALL cannot copy file` / permission denied.

Admin install also writes **All Users** shortcuts:
- Start Menu → **ArchStreamer** (every Windows account)
- Public Desktop → **ArchStreamer**

Use `.\deploy\windows\finish-install.ps1 -Shortcuts -CurrentUserOnly` only if you intentionally want per-user shortcuts.

## Install deps

```powershell
# Elevated recommended (ViGEmBus driver)
.\deploy\windows\install-deps.ps1
# or
.\build_windows.ps1 -InstallDeps
.\build_windows.ps1 -InstallDeps -BuildHost -Reconfigure
```

[`deploy/windows/install-deps.ps1`](install-deps.ps1) handles:

| Dependency | Notes |
|---|---|
| vcpkg Qt6 Widgets + SDL2 | Uses `VCPKG_ROOT` (default `C:\dev\vcpkg`) |
| GStreamer MSVC 64-bit | winget/search + MSI links; needs `d3d11screencapturesrc`, `wasapisrc`, encoders |
| ViGEmBus | Driver via winget; place `ViGEmClient.dll` on `PATH` |
| CMake / Ninja | Optional `-InstallBuildTools` |
| Firewall | Optional `-OpenFirewall` for control/input/RTP ports |
| **Yuzu** | Manual — copy `yuzu-windows-msvc` → `%LOCALAPPDATA%\archstreamer\yuzu\` or set `ARCHSTREAMER_YUZU` |

## Build

```powershell
.\build_windows.ps1                 # client
.\build_windows.ps1 -BuildHost      # host + client GUI Host tab
.\build_windows.ps1 -Clean -BuildHost
```

Requires CMake, vcpkg toolchain, VS Build Tools (or Ninja + MSVC).

## Runtime

| Piece | Role |
|---|---|
| `SDL2.dll` | Copied next to `archstreamer_gui` on build |
| GStreamer MSVC on `PATH` | Client decode + host capture/encode |
| ViGEmBus + `ViGEmClient.dll` | Host virtual Xbox 360 pads |
| Managed Yuzu | `%LOCALAPPDATA%\archstreamer\yuzu\yuzu.exe` (+ DLLs) |
| Keys | `%APPDATA%\yuzu\keys` or `ARCHSTREAMER_YUZU_KEYS` |

## Host capture (Windows)

- Video: `d3d11screencapturesrc` (desktop) → H.264 → RTP
- Audio: `wasapisrc loopback=true` → Opus → RTP
- Pads: ViGEmBus X360 targets bound into Yuzu Controls (same GUID flow as Linux)

## Firewall

Allow inbound TCP **45555**, UDP **45454**, **5004**, **6004**, **45550** (or `install-deps.ps1 -OpenFirewall`).
