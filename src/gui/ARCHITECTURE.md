# src/gui Architecture

`src/gui/` implements the Qt desktop GUI.

The GUI composes reusable host and client services into tabs, dialogs, settings,
update actions, video embedding, QR/form sync, and profile controls.

GUI code may own presentation state and widget wiring. Session protocol,
catalog, input, and host process behavior should stay in reusable libraries so
CLI and GUI flows remain aligned.
