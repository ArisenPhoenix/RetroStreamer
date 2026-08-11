# mobile Architecture

`mobile/` owns mobile client applications.

The Android app connects to the same host protocol as desktop clients, renders
the mobile/TV UI, captures controller/keyboard/remote input, handles QR form
sync, receives media, and reports device/stream feedback to the host.

Shared protocol and control vocabulary should stay aligned with native common
code. Platform-specific Compose/UI and Android input handling belong here.
