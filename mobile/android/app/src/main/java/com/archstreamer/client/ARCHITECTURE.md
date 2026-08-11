# mobile Android Client Package Architecture

`com.archstreamer.client` is the Android client root package.

Top-level classes own application startup, Android device profiling, and
foreground session keepalive. Feature packages own networking, media, pairing,
protocol, Cadence access, and UI.
