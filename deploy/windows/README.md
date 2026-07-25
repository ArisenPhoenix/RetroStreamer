# Windows client

Windows builds are **client-only** today (`-DARCHSTREAMER_BUILD_HOST=OFF`). There is no Windows host, and Linux-only host tools (gamescope, Gamescope WSI, VirtualGL, Yuzu AppImage, uinput, PRIME) do **not** apply.

## Build

Prefer PowerShell:

```powershell
.\build_windows.ps1
# or
.\build_windows.ps1 -Clean
```

Requires:

| Dependency | Role |
|---|---|
| CMake | Configure / build |
| vcpkg (`VCPKG_ROOT`, default `C:\dev\vcpkg`) | Qt6 Widgets, SDL2 |
| Visual Studio / Build Tools (or Ninja + MSVC) | Compiler |
| Optional: Ninja | Faster incremental builds |

`build_windows.ps1` / `build_windows.sh` always pass `-DARCHSTREAMER_BUILD_HOST=OFF`.

## Runtime (client)

| Dependency | Role |
|---|---|
| `SDL2.dll` | Copied next to `archstreamer_gui` on build |
| GStreamer **MSVC 64-bit** on `PATH` | Video/audio receive (`gst-launch-1.0.exe`) |

Preferred GStreamer elements on Windows: `d3d11h264dec`, `d3d11videosink`, `autoaudiosink`. Install the official GStreamer MSVC runtime and dev packages and ensure `gst-launch-1.0.exe` is on `PATH`.

## What did *not* change with recent host work

Gamescope WSI, managed gamescope, VirtualGL, dual-GPU encode/render, and Yuzu launch env are **Linux host** features. They add **no** new Windows client packages, vcpkg ports, or DLLs.

When a Windows **host** exists later, it will need its own backends (ViGEm, DXGI/WGC capture, WASAPI loopback) — see `ARCHITECTURE.md` § Windows Direction — not a port of gamescope.
