# src/host/video/gst

GStreamer encode library and worker process.

`gst_video_engine.cpp` is the only file that includes `gst.h`.
`archstreamer_gst_worker` reads commands from stdin and writes events to stdout.
MediaServer spawns one worker per pipeline and keeps the gst pipeline id from
the Ready event. The worker writes that id to the runtime video record while
the pipeline is playing.
