# Client cleanup and library boundaries plan

## Goals

- Make the client-facing log a concise record of user selections, host responses,
  session state, and actionable problems; retain useful opt-in diagnostics.
- Complete automatic controller-family selection for desktop sessions, matching
  Android's system-based behavior and retaining users' saved family profiles.
- Continue extracting cohesive libraries that serve both GUI and command-line
  clients, with shared Windows/Linux GUI and application logic. Isolate genuine
  platform differences behind small interfaces selected by the build.

## What exists today

- CMake already builds `archstreamer_core`, `archstreamer_controller`,
  `archstreamer_client`, and, when enabled, `archstreamer_host`. Core and client
  still own many unrelated responsibilities; the Qt GUI is compiled directly
  into an executable. Existing platform-specific source selection in CMake and
  the portable GUI with host/client implementation variants are useful patterns.
- `ClientApp::join_session` already looks up the selected game in the full catalog,
  resolves `system_key` through `controller_map_family_from_system_key`, updates
  the live profile, and notifies the GUI. The GUI can subsequently override that
  active family through the family editor. Confirm behavior through the actual
  join, reconnect, and GUI event paths before changing it; the issue may be
  timing, metadata, UI semantics, or a combination.
- `GameInfo` contains `system_key`; `ActiveSessionInfo` contains a selected game
  ID but no system key. Android also derives its family from catalog metadata.
  C++ and Kotlin duplicate the system-key mapping. Both currently match the
  `gameboy` substring before `gameboyadvance`, so a long-form GBA key can resolve
  to GB. Treat aliases and precedence as compatibility behavior to test.
- The desktop GUI has Quiet/Normal/Verbose levels and separate opt-in client
  diagnostic categories. `append_log` filters before writing the shared GUI
  log file, while the client log bundle reads that file. Raw host-process lines
  have a separate path. A cleaner display must account for both visibility and
  the diagnostics actually available in exported logs.
- Existing Android unit tests provide a starting point; CMake does not currently
  register a comparable test suite. The root README identifies limited coverage
  of control mappings, joining, and recovery as existing risks.

## Evidence from Kirin's November 6 session

The host GUI log and Kirin's submitted Windows client bundle show several distinct
issues. Their clocks differ by roughly two seconds; timestamps are approximate.

- Repeated join attempts around 12:20–12:22 were rejected by the host with
  `incorrect password or invalid credentials`, while the client reported only
  `host disconnected`. Catalog retrieval succeeded, so catalog access alone does
  not establish that session authentication succeeded. At the time, the player
  wait path logged candidate failures without sending an error. It now sends an
  `ErrorPacket` that `ClientSessionService::finish_join` can display. Verify the identity and credential path before
  assuming why authentication failed; preserve generic authentication wording.
- Earlier, around 11:35, the host reported clients selecting different games and
  rejected candidates. Correlate individual candidates before attributing every
  rejection to Kirin. A later multiplayer join waited nearly three minutes until
  the host stopped, with no useful waiting status visible to Kirin. The host was
  configured to wait up to 2,000 seconds for players.
- At 11:37 a failed Windows TCP connection was reported as `WSA 0`; the code
  closed the socket before retrieving the Winsock error. It now captures the
  original error before cleanup, and the GUI gives general connection guidance.
- A later multiplayer session did start, and the host recorded nonzero input from
  Kirin. A subsequent session ran until the video window closed. This establishes
  some connectivity and input, not that controls, video quality, or audio were
  correct throughout. The host reported a staged video change timing out during
  the latter session; investigate that separately from authentication.
- For the standalone GStreamer receiver, the reported `decoded` count is derived
  from once-per-second `progressreport` lines, not a frame count. The roughly one
  per second in the log does not demonstrate 1 fps. GUI video health and host
  readiness decisions should use accurately defined metrics, and staging evidence
  should distinguish lack of buffers from an actual low frame rate.
- The client temporarily detected no controllers around 12:21–12:22; it later
  detected and selected the PS4 controller again. The logs do not establish why.
  A GStreamer `Output window was closed` message followed the reported window
  closure and alone does not establish a prior video failure.

## Delivery sequence

### 1. Establish behavior and regression coverage

1. Trace catalog retrieval, host-confirmed selected game ID, session start,
   reconnect and teardown, including viewer and existing-session joins. Identify
   exactly when a current catalog entry may be absent or stale, and whether a
   user can join a game other than their initial selection. Record reproducible
   cases on both desktop platforms where possible.
2. Add focused C++ tests for system-key aliases, case, unknown/empty keys,
   collision precedence, and profile application. Use shared input/expected
   fixtures in Android tests as well, so duplicated implementations cannot drift.
   Fix GBA precedence in both implementations and check other overlapping aliases.
3. Capture representative current client-visible and exported logs for connect,
   successful join, rejected join, disconnect, and optional diagnostics. Use
   these examples as acceptance cases rather than relying on string suppression.
4. Extend the lobby rejection regression test to cover game/mode mismatches and
   waiting-for-players state. The current test covers authentication rejection
   through the client and verifies that the lobby remains available. Preserve
   socket errors across cleanup on every platform.
   Investigate the video staging timeout using aligned host/client diagnostics,
   with progress ticks labeled as such until actual frame counts are available.

### 2. Finish automatic desktop controls

1. Define one session-level resolution path in reusable client code: use the
   host-confirmed game ID and corresponding current catalog system key to choose
   the family before input starts. Keep the current profile document and sync
   format compatible. Where metadata is unavailable, use Standard explicitly,
   report the fallback, and update when reliable metadata becomes available.
2. Separate the family being edited from the family active in a session. Show
   the detected system and active family; editing a different family's saved
   profile must not silently change live controls. Preserve live edits to the
   active profile. If a temporary override is useful, make it explicit and
   session-scoped, with a clear return to automatic selection.
3. Re-resolve on joins, reconnects, and actual game changes; clear session state
   on all exit and error paths. Avoid applying queued updates from an old session
   to a new one. Verify keyboard, physical pads, spectators, and multiple local
   seats are unaffected where applicable.
4. Only extend the wire protocol if tracing shows existing catalog/ID data cannot
   reliably identify the actual system. If needed, add a backward-compatible
   optional field and cover old/new host and client combinations in C++ and
   Android; do not infer the system from a display name without a defined rule.

### 3. Make client logs useful at a glance

1. Define typed client events with origin, severity, and audience/category at
   their source. Default visible events should cover selected host/game/role,
   host acceptance or rejection, assigned seats, readiness, session end, and
   actionable errors. Include automatic controller selection and fallback when
   useful. Coalesce repeated progress; avoid pipeline descriptions, endpoint
   internals, and routine polling in the normal view.
2. Give the visible log and diagnostic file/bundle independent filtering: retain
   errors and enough context for support, and keep detailed control, connection,
   video, and audio telemetry opt-in. Preserve the existing Send logs workflow,
   session markers, bounded log handling, and host acknowledgement. Review
   sensitive information and avoid increasing what is collected by default.
3. Route CLI and Qt through the same event semantics while letting each present
   them appropriately. Keep host and remote logs useful and separate in the UI;
   do not discard host errors because a text tag resembles a noisy subsystem.
   Test representative events, filtering, ordering, and exported bundle contents.

### 4. Extract libraries along demonstrated ownership boundaries

1. Map current source dependencies and public headers before moving files. Start
   with the code exposed by the preceding work: portable control-family/profile
   resolution and client session events, then session/catalog services and media
   interfaces where they have independent consumers. Keep dependency direction
   from applications and Qt presentation toward client services, then shared
   protocol/data and platform interfaces.
2. Give each extracted target a narrow public API, private implementation and
   dependencies, and independently buildable tests. Split targets only when they
   have a coherent responsibility or enable a real consumer/test; avoid circular
   dependencies and merely wrapping existing monoliths in more targets.
3. Move reusable GUI behavior into shared Qt components or a Qt library only
   where there are actual reusable boundaries. Keep host capability optional as
   it is today. Introduce platform adapters for OS-specific process, window,
   audio, input, or filesystem operations, with CMake choosing implementations;
   preserve one desktop workflow and shared widgets. Keep unavoidable conditional
   compilation localized to adapter boundaries and dependency detection.
4. Migrate incrementally, keeping the GUI, CLI, host-enabled and client-only
   builds working after each extraction. Update architecture docs as ownership
   changes; retain existing deployment and packaging behavior.

## Validation and completion criteria

- Automated tests cover alias precedence and shared C++/Kotlin fixtures; selected
  game resolution, unknown metadata, live family edits, reconnect and stale GUI
  callbacks; plus event filtering and diagnostic bundle contents.
- Build and exercise the Linux and Windows desktop clients, including the Qt GUI
  and CLI; test host-enabled and client-only configurations where supported.
  Document any platform or hardware that cannot be exercised instead of assuming
  success. Run Android unit tests and check controls sync compatibility.
- In a real host session, select games from at least GB/GBC, GBA, DS/3DS,
  Switch, PSX, and an unknown/standard system, including an existing-session join.
  Verify the actual sent controls use the right saved profile, that editing other
  families does not change gameplay, and that session transitions reset correctly.
- Normal client logs tell the story of selections and host responses without
  routine media/input noise; failures remain actionable and opt-in diagnostics
  remain available in the exported bundle.

## Decisions to settle during implementation

- Whether an explicit session-only override is needed, and how it is presented.
- Whether the catalog and confirmed game ID are sufficient for every join and
  reconnect path, or an optional host-provided system key is justified.
- Which measured seams warrant separate targets after the first behavior changes;
  target names and the final dependency graph should follow those seams.
