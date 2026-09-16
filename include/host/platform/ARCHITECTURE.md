# include/host/platform Architecture

`include/host/platform/` contains narrow platform abstraction headers that do not
fit a more specific host bucket.

Prefer `host/video/`, `host/audio/`, `host/virtual/`, `host/session/`,
`host/console/`, or `host/hardware/` when a platform boundary clearly belongs
to one of those concerns.
