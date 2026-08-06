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
- Controller remaps + overlay profiles in local SQLite
  (`databases/archstreamer_cadence.sqlite`, `user_controls` kinds
  `button_map` / `overlay_profiles`). SharedPreferences caches overlays after load.
  First launch imports legacy `controller_button_map.json` and prefs overlays into SQL.
- Manual **Pull from host** / **Push to host** (Game Options) exchanges a binary
  `controls.sqlite` pack over TCP (`ControlsDbPull`/`Push`). Requires an authenticated
  connection (LobbyPresence or live session) and a real save-profile username — the
  local “android” placeholder is never transported. On the host the file lives at
  `saves/<username>/controls.sqlite`. Desktop reuses `button_map`; mobile reuses
  overlays; same schema across devices. Controls editing is locked until a profile
  username is set.

## Smoke: remaps survive process death

1. Connect, open Game Options, change a button remap, leave the screen.
2. Force-stop the app (or kill process); relaunch.
3. Remaps should match. Confirm with:
   `adb shell run-as com.archstreamer.client ls databases/`
   (expect `archstreamer_cadence.sqlite`; JSON file may remain as a one-shot backup).

## Smoke: host controls sync

1. Sign in with a save-profile username (e.g. `alina`), connect catalog (or play).
2. Game Options → **Push to host**, then change a remap/overlay on another device and
   **Pull from host** (or the reverse).
3. Host file: `~/.local/share/archstreamer/saves/<user>/controls.sqlite`.

## Not yet

- Opus audio decode
- Soft keyboard OSK for Ryujinx prompts
- LAN discovery / art download
- Stream quality / size picker (defaults Medium / 720p in heartbeats)

## Host reminder

Phone and PC must be on the **same Wi‑Fi**. Host must be running the lobby (`host_runner` / GUI Host) with matching ports. Username should match a save profile you use on desktop (e.g. `alina`).
