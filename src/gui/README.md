# src/gui

Qt GUI implementation lives here.

This directory owns desktop GUI widgets, tabs, dialogs, update flow, local
video embedding, host/client form handling, and GUI-specific helpers.

Reusable host or client behavior should stay in `src/host/` or `src/client/` so
the CLI tools and GUI can share it.
