# include/host/video/gst

GStreamer encode commands, line codec, and engine API.

`MediaServer` may call `GstVideoEngine` in-process (raw start description).
`archstreamer_gst_worker` speaks the full MediaServer video command set over
stdin/stdout. Neither header includes `gst.h`.
