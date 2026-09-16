# src/host/session Architecture

`src/host/session/` implements active session runtime behavior.

This includes launch assembly, backend preparation, active slot lifecycle,
control monitoring, media endpoint ownership, reconnect state, and teardown.

The session layer is the runtime owner once play starts. It should receive
validated input from lobby and plug `host/contracts/` types into console, video,
audio, and virtual helpers.

`SessionManager::start_session` waits for the same user's previous slot to
finish when they switch games (or when that slot is already stopping), then
reaps leftover emulator tokens and stale X reservation locks before claiming a
new slot.
