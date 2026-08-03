# ArchStreamer Android client (Kotlin / Jetpack Compose)

Open **this folder** (`mobile/android`) in Android Studio — not the C++ repo root.

## First open

1. Android Studio → **Open** → select `mobile/android`
2. Let Gradle sync (downloads SDK deps if needed)
3. If prompted for SDK, install **API 35** + Build-Tools
4. Plug in a phone (USB debugging) or start an emulator
5. Run the `app` configuration

`local.properties` is created by Studio with your SDK path; it is gitignored.

## What works in v0.1+

- Connect to host LAN IP (default control `45555`, input `45454`)
- Fetch game catalog over TCP (`GameListRequest` / `GameList`)
- Start a single-player session (`ClientHello` → welcome / seats / ready)
- RTP H.264 receive + MediaCodec decode to fullscreen Surface
- Viewer heartbeats (keeps the host session alive)
- On-screen gamepad overlay → UDP `ControllerInput`
- Optional physical USB/Bluetooth gamepad (Game Options); Home/Guide opens the play menu

## Not yet

- Opus audio decode
- Soft keyboard OSK for Ryujinx prompts
- LAN discovery / art download
- Stream quality / size picker (defaults Medium / 720p in heartbeats)

## Host reminder

Phone and PC must be on the **same Wi‑Fi**. Host must be running the lobby (`host_runner` / GUI Host) with matching ports. Username should match a save profile you use on desktop (e.g. `alina`).
