# include/host/platform Architecture

`include/host/platform/` contains narrow platform abstraction headers that do not
fit a more specific host bucket.

Prefer `host/hardware/`, `host/virtual/`, `host/session/`, or `host/console/`
when a platform boundary clearly belongs to one of those concerns.
