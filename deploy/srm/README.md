# Steam ROM Manager ↔ ArchStreamer art

Steam ROM Manager scrapes artwork. ArchStreamer **never** sends art over the wire;
each computer reads local files under `Art/`.

## Install / launch

```bash
./scripts/install-srm.sh          # already done if AppImage exists
./scripts/launch-srm.sh
```

AppImage path: `/mnt/Internal_SSD/Gaming/tools/srm/Steam-ROM-Manager.AppImage`

## One-time SRM settings

1. **Settings → Environment Variables**
   - **ROMs Directory:** `/mnt/Internal_SSD/Gaming/ROMS/Games`
   - **Local Images Directory:** `/mnt/Internal_SSD/Gaming/ROMS/Art`
   - **Steam Directory:** your Steam install (required for saving shortcuts; art preview still works without it, but SRM is designed around Steam)
2. Create parsers (or use presets) for systems you care about, e.g. GB → `.../Games/GB`.
3. In each parser **Local Artwork**:
   - poster: `${localImagesDir}/poster/${title}.@(png|jpg|jpeg|webp)`
   - hero: `${localImagesDir}/heroes/${title}.@(png|jpg|jpeg|webp)`
   - logo: `${localImagesDir}/logos/${title}.@(png|jpg|jpeg|webp)`
   - icon: `${localImagesDir}/icons/${title}.@(png|jpg|jpeg|webp)`
4. Enable **DRM Protect** (artwork backup) so SGDB choices persist under
   `~/.config/steam-rom-manager/userData/artworkBackups/`.
5. Preview → pick artwork → save. Prefer also writing into the Local Images folders above.

## Sync into ArchStreamer catalog paths

```bash
./scripts/sync_srm_art_into_catalog.sh
```

This copies `Art/poster/<ROM title>.png` (etc.) into:

```text
Art/<asset_key>/boxart.png
Art/<asset_key>/grid.png
Art/<asset_key>/hero.png
...
```

The GUI resolves those paths (and will also read SRM title folders directly as a fallback).

## Placeholder

Until a game has art: `Art/default/default_image.png`

## Steam userdata stub

SRM needs a Steam account `userdata/.../config/shortcuts.vdf`. On machines without
a full Steam library yet, launch runs:

```bash
./scripts/ensure_srm_steam_layout.sh
```

Behavior:
- If `shortcuts.vdf` already exists and looks valid → leave it alone
- If missing/empty/corrupt → create a minimal valid empty VDF (does not wipe real Steam data)
- Always ensures `userdata/<account>/config/grid` exists

Account/path resolution order:
1. `ARCHSTREAMER_STEAM_ACCOUNT_ID` / `ARCHSTREAMER_STEAM_DIR`
2. SRM `userSettings.json` environment variables
3. Auto-detect best numeric `userdata/<id>` (grid + shortcuts score)
4. Stub account `0` only if nothing else exists

In the ArchStreamer GUI **Settings** tab, set **Steam account ID** (or leave blank / click Detect)
and use **Refresh Art from Steam**. Values persist in Qt settings.
