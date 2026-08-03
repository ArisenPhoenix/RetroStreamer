#!/usr/bin/env python3
"""Copy SRM title-based local art into ArchStreamer asset_key folders."""

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
from pathlib import Path

from scriptutil import (
    REL_ART_ROOT,
    REL_META_ROOT,
    REL_ROM_ROOT,
    add_gaming_root_arg,
    env_path,
    eprint,
    repo_root,
    resolve_gaming_path,
)


def normalize(value: str) -> str:
    value = value.casefold()
    value = value.replace("é", "e").replace("pokémon", "pokemon")
    return re.sub(r"[^a-z0-9]+", "", value)


def main(argv: list[str] | None = None) -> int:
    root = repo_root()
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="Reference implementation: scripts/sync_srm_art_into_catalog.sh",
    )
    add_gaming_root_arg(parser)
    parser.add_argument(
        "--art-root",
        type=Path,
        default=env_path("ARCHSTREAMER_ART_ROOT"),
        help="Art root (default: <gaming-root>/ROMS/Art)",
    )
    parser.add_argument(
        "--rom-root",
        type=Path,
        default=env_path("ARCHSTREAMER_ROMS_ROOT"),
        help="ROM root (default: <gaming-root>/ROMS/Games)",
    )
    parser.add_argument(
        "--meta-root",
        type=Path,
        default=env_path("ARCHSTREAMER_META_ROOT"),
        help="Meta root (default: <gaming-root>/ROMS/Meta)",
    )
    parser.add_argument(
        "--build-dir",
        type=Path,
        default=root / "build",
    )
    args = parser.parse_args(argv)

    art_root = resolve_gaming_path(
        gaming_root=args.gaming_root,
        relative=REL_ART_ROOT,
        override=args.art_root,
        label="art root (--art-root)",
    )
    rom_root = resolve_gaming_path(
        gaming_root=args.gaming_root,
        relative=REL_ROM_ROOT,
        override=args.rom_root,
        label="ROM root (--rom-root)",
    )
    meta_root = resolve_gaming_path(
        gaming_root=args.gaming_root,
        relative=REL_META_ROOT,
        override=args.meta_root,
        label="meta root (--meta-root)",
    )
    asset_probe = args.build_dir / "asset_probe"

    if not (asset_probe.is_file() and os.access(asset_probe, os.X_OK)):
        eprint(f"Build asset_probe first: cmake --build {args.build_dir} -j$(nproc)")
        return 1

    output = subprocess.check_output(
        [str(asset_probe), str(rom_root), str(meta_root), str(art_root)],
        text=True,
    )

    mapping = {
        "poster": ("boxart.png", "grid.png"),
        "grids": ("grid.png",),
        "heroes": ("hero.png",),
        "logos": ("logo.png",),
        "icons": ("icon.png",),
        "boxart": ("boxart.png",),
    }

    title_files: dict[str, dict[str, Path]] = {}
    for folder in mapping:
        directory = art_root / folder
        if not directory.is_dir():
            continue
        for path in directory.iterdir():
            if path.is_file() and path.suffix.lower() in {
                ".png",
                ".jpg",
                ".jpeg",
                ".webp",
            }:
                title_files.setdefault(normalize(path.stem), {})[folder] = path

    copied = 0
    matched_games = 0
    unmatched: list[str] = []

    def flush_entry(entry: dict[str, str]) -> None:
        nonlocal copied, matched_games
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
                print(
                    f"copied {src.relative_to(art_root)} -> {dest.relative_to(art_root)}"
                )
                copied += 1

    current: dict[str, str] | None = None
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

    print(
        f"done: matched_games={matched_games} files_copied={copied} unmatched={len(unmatched)}"
    )
    if unmatched:
        print("unmatched titles (add under Art/poster/<title>.png):")
        for title in unmatched[:20]:
            print(f"  - {title}")
        if len(unmatched) > 20:
            print(f"  ... and {len(unmatched) - 20} more")
    return 0


if __name__ == "__main__":
    sys.exit(main())
