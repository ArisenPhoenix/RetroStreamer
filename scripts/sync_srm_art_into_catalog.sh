#!/usr/bin/env bash
# Copy SRM title-based local art into ArchStreamer asset_key folders.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${ROOT}/build"
ART_ROOT="${ARCHSTREAMER_ART_ROOT:-/mnt/Internal_SSD/Gaming/ROMS/Art}"
ROM_ROOT="${ARCHSTREAMER_ROMS_ROOT:-/mnt/Internal_SSD/Gaming/ROMS/Games}"
META_ROOT="${ARCHSTREAMER_META_ROOT:-/mnt/Internal_SSD/Gaming/ROMS/Meta}"

if [[ ! -x "${BUILD}/asset_probe" ]]; then
  echo "Build asset_probe first: cmake --build ${BUILD} -j\$(nproc)" >&2
  exit 1
fi

python3 - "${ART_ROOT}" "${ROM_ROOT}" "${META_ROOT}" "${BUILD}/asset_probe" <<'PY'
import pathlib
import re
import shutil
import subprocess
import sys

art_root = pathlib.Path(sys.argv[1])
rom_root = pathlib.Path(sys.argv[2])
meta_root = pathlib.Path(sys.argv[3])
asset_probe = sys.argv[4]

output = subprocess.check_output(
    [asset_probe, str(rom_root), str(meta_root), str(art_root)],
    text=True,
)

def normalize(value):
    value = value.casefold()
    value = value.replace("é", "e").replace("pokémon", "pokemon")
    return re.sub(r"[^a-z0-9]+", "", value)

mapping = {
    "poster": ("boxart.png", "grid.png"),
    "grids": ("grid.png",),
    "heroes": ("hero.png",),
    "logos": ("logo.png",),
    "icons": ("icon.png",),
    "boxart": ("boxart.png",),
}

title_files = {}
for folder in mapping:
    directory = art_root / folder
    if not directory.is_dir():
        continue
    for path in directory.iterdir():
        if path.is_file() and path.suffix.lower() in {".png", ".jpg", ".jpeg", ".webp"}:
            title_files.setdefault(normalize(path.stem), {})[folder] = path

current = None
copied = 0
matched_games = 0
unmatched = []

def flush_entry(entry):
    global copied, matched_games
    asset_key = entry.get("asset_key")
    display = entry.get("display_name", "")
    if not asset_key or not display:
        return

    candidates = [normalize(display)]
    canonical = entry.get("canonical_name", "")
    if canonical:
        candidates.append(normalize(canonical))
        candidates.append(normalize(canonical.replace("-", " ")))

    matched = None
    for candidate in candidates:
        if candidate in title_files:
            matched = title_files[candidate]
            break
    if matched is None:
        unmatched.append(display)
        return

    matched_games += 1
    dest_dir = art_root / asset_key
    dest_dir.mkdir(parents=True, exist_ok=True)
    for folder, targets in mapping.items():
        src = matched.get(folder)
        if src is None:
            continue
        for target_name in targets:
            dest = dest_dir / target_name
            if dest.exists() and dest.stat().st_mtime >= src.stat().st_mtime:
                continue
            shutil.copy2(src, dest)
            print("copied {} -> {}".format(src.relative_to(art_root), dest.relative_to(art_root)))
            copied += 1

for line in output.splitlines():
    if line.startswith("Assets root:") or line.startswith("Found "):
        continue
    if not line.startswith(" "):
        if current is not None:
            flush_entry(current)
        current = {"display_name": line.strip()}
        continue
    if current is None:
        continue
    stripped = line.strip()
    if stripped.startswith("asset_key="):
        current["asset_key"] = stripped.split("=", 1)[1]
    elif stripped.startswith("canonical_name="):
        current["canonical_name"] = stripped.split("=", 1)[1]

if current is not None:
    flush_entry(current)

print("done: matched_games={} files_copied={} unmatched={}".format(matched_games, copied, len(unmatched)))
if unmatched:
    print("unmatched titles (add under Art/poster/<title>.png):")
    for title in unmatched[:20]:
        print("  - {}".format(title))
    if len(unmatched) > 20:
        print("  ... and {} more".format(len(unmatched) - 20))
PY
