# include/host/video/gst Architecture

Command types for a GStreamer video encode process.

`GstVideoCommand` verbs match MediaServer video ops already in use:
start/stop, reconfigure, apply-layout, add-sink/remove-sink, staging, status.

`GstVideoEngine` runs them. `archstreamer_gst_worker` is the same engine over
stdin/stdout. A playing pipeline is named `asv-<pid>-<n>` and that id is the
first line of Ready/Status (`id=...`) plus a record at
`gst_video_pipeline_record_path`. Mutating verbs currently relaunch
parse-launch — the names exist so live pad surgery has a place to land.
Session spawns one worker per pipeline.
