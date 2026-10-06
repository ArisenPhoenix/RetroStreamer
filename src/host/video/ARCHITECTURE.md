# src/host/video Architecture

`src/host/video/` implements present surfaces and video capture/encode.

Xvfb/Xephyr startup waits until the Unix socket accepts a connection, not merely
until `/tmp/.X11-unix/Xn` exists, so `ximagesrc` does not race keymap compile.

In-process `ximagesrc` retries the same way PipeWire does, so a new RetroArch
session after a Switch/gamescope session can open `:99` once Xvfb is actually
accepting connections.

Property quoting differs by runner. In-process pipelines are one
`gst_parse_launch` string, so values use double quotes (`display-name=":99"`);
the parser leaves single quotes in the property and `XOpenDisplay` fails.
The gst-launch child builds argv tokens (`display-name=:99`) and must not wrap
simple values — gst-launch joins those tokens, then parse-launchs the result.

`GStreamerMediaServer` remains the session-facing A/V facade. It owns
`MediaPlanConfig`; session fills that from `MediaConfig` and passes it to
`start()`. Audio encode lives in `src/host/audio/gstreamer_audio_fanout.cpp`.
Managed parse-launch and bus watching live in `src/host/video/gst/`
(`archstreamer_gst`). MediaServer starts a `GstVideoGraph` (source + trunk
branch). Below-trunk clients get their own sample encode so the trunk pipeline
is not relaunched. Sessions spawn `archstreamer_gst_worker` (one process per
pipeline). `ARCHSTREAMER_VIDEO_PIPELINE=managed` keeps the in-process engine.
