# Runtime Cadence

Control-plane store for **user lookups** and **structured runtime events** (who played what, host start, race notes). Not for pad/media hot path.

## Build switch

```bash
# Default — file backend (users.json + day JSONL). No sidecar.
cmake -S . -B build -DARCHSTREAMER_RUNTIME_CADENCE=file

# SQLite sidecar + Unix-socket client
cmake -S . -B build -DARCHSTREAMER_RUNTIME_CADENCE=db
```

Compile definitions:

- `ARCHSTREAMER_RUNTIME_CADENCE_FILE=1` (default)
- `ARCHSTREAMER_RUNTIME_CADENCE_DB=1`

`archstreamer::cadence::ActiveStore` and `make_runtime_store()` select the implementation at build time.

## Users / credentials

Cadence is the **authoritative** store for usernames and passwords on every platform
that builds against `RuntimeStore` (file or db).

| Field | Meaning |
|-------|---------|
| `username` | Primary key (`[A-Za-z0-9_-]`, 1–64) |
| `display_name` | Optional label |
| `password_hash` | `v1:<salt>:<sha256(salt:password)>` (legacy plaintext accepted once, then upgraded) |
| `must_change` | Forced password change on next join |
| `profile_path` | Absolute save profile dir: `<save_root>/<username>` |
| `save_root` | Host save-root used when the row was written |
| `created_at` / `updated_at` | Unix epoch seconds |

Portable helpers live in `archstreamer/runtime_cadence/user_auth.hpp`
(`verify_or_create_user`, `change_user_password`, `import_users_from_save_root`,
`backfill_user_profile_paths`, …).

Host join still creates **save profile directories** under the save root for game blobs.
`credentials.json` is dual-written as a legacy mirror only.

On host start (db/file), existing `<save-root>/<user>/credentials.json` rows are
imported into cadence when missing, and empty `profile_path` values are backfilled
when `<save_root>/<username>` exists.

## User controls (button map)

Portable controller remaps (`ControllerMapDocument` JSON — same schema as
`shared/controller_button_map.json`) live in `user_controls`, keyed by username.

| Field | Meaning |
|-------|---------|
| `username` | Profile / client username (`_default` for pre-profile device-wide import) |
| `kind` | `button_map` for now |
| `document_json` | Full map document JSON |
| `version` | Document version (currently `1`) |
| `updated_at` | Unix epoch seconds |

Desktop GUI and Android load/save through `upsert_controls` / `find_controls`.
Host cadence (file or SQLite sidecar) and Android local
`archstreamer_cadence.sqlite` use the **same** table/column names and JSON body.
Overlay chrome (touch layout) stays device-local (SharedPreferences) for now.

Ops: `upsert_controls`, `find_controls`, `list_controls`.

File backend: `controls.json` object keyed by `username\\nkind`.

## Query examples (db)

```bash
sqlite3 ~/.local/share/archstreamer/cadence/cadence.sqlite
```

```sql
-- Save-profile lookup for Saves tooling
SELECT username, profile_path, save_root FROM users;

-- Controller remaps after editing Game Options on desktop
SELECT username, kind, version, updated_at,
       length(document_json) AS json_bytes
FROM user_controls
WHERE kind = 'button_map';
```

Android (after editing remaps on device):

```text
adb shell run-as com.archstreamer.client \
  sqlite3 files/archstreamer_cadence.sqlite \
  "SELECT username, kind, length(document_json) FROM user_controls;"
```

Ops: `upsert_user`, `find_user`, `delete_user`, `list_users`, `ping`, `record_event`, `recent_events`.

## Paths

| Cadence | Location |
|---------|----------|
| Data root | `~/.local/share/archstreamer/cadence/` |
| File users | `…/users.json` |
| File controls | `…/controls.json` |
| File events | `…/events_YYYY-MM-DD.jsonl` |
| SQLite | `…/cadence.sqlite` (WAL) |
| Socket | `$XDG_RUNTIME_DIR/archstreamer/cadence.sock` (else under data root) |
| Android local | `filesDir/archstreamer_cadence.sqlite` (`user_controls`) |

Day event tables in SQLite: `events_YYYY_MM_DD`.

## Related: game metadata

Game titles / secondary ids (Switch title-id, content stems, …) live in a **separate**
SQLite DB — see [docs/game_meta.md](../docs/game_meta.md)
(`~/.local/share/archstreamer/meta/games.sqlite`). Not part of cadence.

## Session events

Recorded via `host/cadence_session_events.hpp` (soft-fail; never blocks play).

| Kind | When | Fields |
|------|------|--------|
| `host_started` | Host process ready | `host_id`=pid, `detail`=host_runner |
| `session_started` | Emulator verified live | `slot`, `username`=save user, `game_key`, `detail`=mode |
| `session_ended` | Session teardown | same + `detail`=end reason |
| `client_joined` | Initial seat, late join, reconnect | `username`=client, `detail`=player\|viewer\|host\|reconnect |
| `client_left` | Viewer removed / player disconnect | `detail`=left\|disconnected\|heartbeat timed out\|… |

`username` on session_* is the save-profile owner; join/leave use the client hello username.

## Sessions + resource claims

Live inventory for handoff / availability (file: `sessions.json` + `claims.json`;
db: `sessions` + `resource_claims` tables).

| Resource type | Example name |
|---------------|--------------|
| `slot_lock` | `slot-0` |
| `pulse_sink` | `archstreamer-0` |
| `pulse_app_id` | `archstreamer-slot-0` |
| `display` | `:99` |
| `video_port` / `audio_port` / `netcmd_port` | `5004` / `6004` / `55355` |
| `emulator_pid` | `12345` |
| `pad_product_base` | `0xa517` |

Ops: `upsert_session`, `end_session`, `find_session`, `list_sessions`,
`claim_resource`, `release_resource`, `release_session_resources`,
`list_claims`, `find_held_resource`.

Held claims have `released_at=0`. Query held inventory to see what can be handed off.

## Instance ops

`reap_stale_instance_state(store, current_host_id)` runs on host start: ends active
sessions / releases held claims whose `host_id` PID is dead, plus orphan claims.

`release_host_instance_state(store, host_id, reason)` runs on clean host exit and
writes a `host_stopped` event.

## Sidecar

Binary: `archstreamer_cadence`

```text
archstreamer_cadence --socket <path> --db <path>
```

When cadence is `db`, `DbRuntimeStore::ensure_ready()` spawns this binary next to `host_runner` if the socket is not already accepting connections.

## Protocol

Length-prefixed JSON (`uint32` big-endian length + UTF-8 object).
Ops include: `ping`, `upsert_user`, `find_user`, `delete_user`, `list_users`,
`upsert_controls`, `find_controls`, `list_controls`,
`upsert_session`, `end_session`, `find_session`, `list_sessions`,
`claim_resource`, `release_resource`, `release_session_resources`,
`list_claims`, `find_held_resource`, `record_event`, `recent_events`.

## Dependencies (db cadence)

Prefer system `sqlite3` via pkg-config (`libsqlite3-dev` / `sqlite-devel`).
If missing, CMake FetchContent downloads the official SQLite amalgamation.
