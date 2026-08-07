# Game metadata store

Durable host-side lookup index for game titles and secondary ids. Separate from
cadence (auth / sessions / events).

## Path

| Item | Location |
|------|----------|
| SQLite DB | `~/.local/share/archstreamer/meta/games.sqlite` (WAL) |
| Fallback (no home) | `./archstreamer-meta/games.sqlite` |

## Authority

- **Identity owner:** this database (`game_meta` + `game_aliases`). Catalog edits update
  the DB first, then rewrite `ROMS/Meta/*.json` so path scans cannot resurrect stale ids.
- **Directory consumer:** catalog scan discovers ROMs and new titles only. For known
  files it binds by `content_path` / scanned id / stem aliases and refreshes
  `content_path` (and launch metadata) without changing edited identity fields.
- Wire `GameInfo` is unchanged; clients receive titles on `GameList` from the
  abridged `catalog_offerings` snapshot (hash/revision matched). After auth the
  host sends `CatalogUserBlocks` (blocked ids only); clients hide those locally
  without rewriting the shared catalog cache.

## Schema

### `game_meta`

Primary key `game_id` (= catalog `GameInfo.id`).

Columns: `system_key`, `system_name`, `display_name`, `canonical_name`, `core_name`,
`asset_key`, `identity_key`, `version`, `language`, `region`, `content_stem`,
`content_path` (absolute ROM path last seen by scan), `updated_at`,
`source` (`catalog` / `meta_json` / `edit` / future `ps2_memcard`).

### `game_aliases`

Primary key `(alias_kind, alias_value, system_key)` → `game_id`.

| Kind | Pass | Meaning |
|------|------|---------|
| `catalog_id` | 1 | Catalog SHA-256 id |
| `canonical` | 1 | Canonical slug |
| `content_stem` | 1 | Lowercased ROM/content basename |
| `display_name` | Lowercased display title |
| `title_id` | Switch 16-hex title id |
| `serial` | Reserved — PS2 product codes |
| `ps2_product` | Reserved — memcard product extraction |

### `user_games`

Per-user catalog associations (primary for PS2 Users rows):

| Column | Meaning |
|--------|---------|
| `username` + `game_id` | Primary key |
| `system_key` | e.g. `ps2` |
| `last_played_at` | Updated when a session starts |

PS2 save-browser keys look like `ps2:id:<catalog_game_id>`. Memcard files are not
listed as games; listing still comes from `user_games`. When `Mcd001.ps2` /
`Mcd002.ps2` are readable, the Users size column shows **game / card capacity**
(`x/y`) from a read-only in-process parse (file payloads + `icon.sys` titles for
matching). Soft-fail if the image is missing or unreadable.

GameCube / Wii Users rows come from Dolphin under `saves/dolphin-emu/User/`:
- **GameCube:** `.gci` files on virtual memcards (`User/GC/…`). Shared `SRAM.raw` is
  ignored (console state, not a game). Keys: `gamecube:gci:<GAMECODE>`.
- **Wii:** NAND title dirs `User/Wii/title/<high>/<low>/` when present. Keys:
  `wii:title:<high>/<low>`.
- Both also surface `user_games` associations (`gamecube:id:…` / `wii:id:…`) after play,
  same pattern as PS2.

### `user_game_blocks`

Per-user catalog hides (Users tab → Block game). Host query is
`SELECT game_id FROM user_game_blocks WHERE username=?`. After auth the host sends
`CatalogUserBlocks` with a content-hash revision: matching client revision → empty
unchanged reply; mismatch → full id list. Clients cache blocks separately from
`catalog_offerings` so each side only ships when its own hash changes. A crafted
join of a blocked id is still rejected on the host.

| Column | Meaning |
|--------|---------|
| `username` + `game_id` | Primary key |
| `system_key` | Cached system (optional) |
| `created_at` | When the block was written |

### `catalog_offerings` / `catalog_offerings_state`

Abridged client catalog snapshot rebuilt at host start from the hosted scan list.
`GameListRequest` with a matching `client_catalog_revision` gets an empty
unchanged reply (`full=false`); otherwise the host sends the full offerings rows.
`content_hash` (sha256 of the canonical wire payload) drives `revision`.

Use `filter_game_list_for_user` / `block_user_game` / `unblock_user_game`. A crafted
join of a blocked id is treated like an unknown game.

## API

```cpp
#include "host/game_meta_store.hpp"

auto store = archstreamer::make_game_meta_store();
if (store->ready()) {
    store->sync_from_catalog(catalog);
    auto row = store->resolve("0100…", "switch");
    auto hints = store->save_name_hints(); // for Users / save browser
}
```

| Call | Role |
|------|------|
| `upsert` / `upsert_alias` | Write one row / alias |
| `find_by_id` | Exact catalog id |
| `find_by_content_path` | Exact ROM path |
| `resolve(key, system_key?)` | Id or any alias (scoped then global) |
| `bind_scanned_game` | Path/id/stem bind; keep DB identity; fold stale clones |
| `write_meta_sidecar` | Persist DB identity into `ROMS/Meta/*.json` |
| `sync_from_catalog` | Prefer `bind_scanned_game` for each hosted path |
| `save_name_hints` | Flatten aliases into `SaveNameHints` |
| `sync_game_meta_from_catalog` | Soft-fail helper (optional / legacy callers) |
| `record_user_game` / `list_user_games` | Per-user catalog game associations |
| `record_user_game_played` | Soft-fail helper (session start) |

Soft-fail: open/write failures must not break play or the Users tab.

## Sync trigger

`scan_game_catalog` calls `bind_scanned_game` per ROM before `add_game`, so the
in-memory catalog already carries DB-owned ids. Users tab Refresh (rescans)
therefore refreshes paths without recreating edited identities. Catalog edits
call `write_meta_sidecar` after filesystem renames (saves, art, DLC, and ROM
basename + Meta sidecar aligned to `display_name`).

`list_save_games` also merges meta hints so Switch title-id leaves and file stems
resolve even without a fresh in-memory catalog map.

Switch title-ids are also learned from each user's Ryujinx
`games/<title_id>/gui/metadata.json` (`title` field) via `learn_switch_title_id`,
preferring the unversioned catalog entry when both base and update builds exist.
Harvest is ingest-only; Users/save listing displays titles via `resolve` / hints,
not by reading Ryujinx at display time.

Active↔save binding and file-save labels also prefer `resolve` (exact path/stem
equality only when meta has no row yet). Parent-folder / extension heuristics
remain a last resort for system key when no alias exists.

## Follow-ups

1. Optional: PS2 `serial` / `ps2_product` aliases from ISO/CHD metadata (sizes already
   match via `icon.sys` titles when aliases are absent).
2. **Gamecube/Wii SRAM:** clean up noisy SRAM rows in the Users tree (deferred).
3. Optional: sniff Dolphin virtual memcard capacity for GameCube `x/y` like PS2.
