# src/host/hardware Architecture

`src/host/hardware/` implements physical host platform behavior.

This includes capture backends, GStreamer media senders, audio sinks, GPU
selection, launch environment setup, local controller bridging, and OS-specific
hardware adapters.

It should expose capabilities to session code without deciding lobby or gameplay
policy.
