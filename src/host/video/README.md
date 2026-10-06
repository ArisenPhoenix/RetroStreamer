# src/host/video

Present-surface and video pipeline implementation lives here.

This area implements virtual displays, capture-plan selection, PipeWire node
wait, and GStreamer/DXGI video senders. GST parse-launch/bus is
`src/host/video/gst/` (`archstreamer_gst` / `archstreamer_gst_worker`).
