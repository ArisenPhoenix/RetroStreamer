# Android cadence Package Architecture

`cadence/` owns Android-side access to Cadence-backed control/settings data.

It should present a small storage/sync API to UI code and avoid knowing about
Compose widgets.
