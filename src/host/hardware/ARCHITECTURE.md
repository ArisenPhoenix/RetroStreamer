# src/host/hardware Architecture

`src/host/hardware/` implements physical host platform behavior.

This includes capture backends, GStreamer media senders, audio sinks, GPU
selection, launch environment setup, local controller bridging, and OS-specific
hardware adapters.

It should expose capabilities to session code without deciding lobby or gameplay
policy.

In-process `ximagesrc` capture retries the same way PipeWire does, so a new
RetroArch session after a Switch/gamescope session can open `:99` once Xvfb is
actually accepting connections.

Property quoting differs by runner. In-process pipelines are one
`gst_parse_launch` string, so values use double quotes (`display-name=":99"`);
the parser leaves single quotes in the property and `XOpenDisplay` fails.
The gst-launch child builds argv tokens (`display-name=:99`) and must not wrap
simple values — gst-launch joins those tokens, then parse-launchs the result.
