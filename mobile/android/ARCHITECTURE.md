# mobile/android Architecture

`mobile/android/` is the Android app module.

The app uses Kotlin/Compose for phone, tablet, and TV UI; Android input APIs for
gamepad, keyboard, remote, and overlay controls; MediaCodec-backed RTP playback;
and native/JNI helpers where shared C++ behavior is required.

Package ownership:

- `cadence/`: Android access to Cadence-backed settings or control data.
- `media/`: RTP H.264/Opus parsing and playback.
- `net/`: host discovery, control connection, logging, and remote-host helpers.
- `pair/`: QR/direct/fallback form sync.
- `protocol/`: Android protocol packet model/codec.
- `ui/`: Compose UI, input routing, overlays, focus, and video surface handling.
