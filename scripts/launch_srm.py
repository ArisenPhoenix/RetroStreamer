#!/usr/bin/env python3
"""Launch Steam ROM Manager with ArchStreamer art defaults."""

from __future__ import annotations

import argparse
import os
import sys
from pathlib import Path

from scriptutil import (
    REL_ART_ROOT,
    REL_ROM_ROOT,
    REL_SRM_APPIMAGE,
    add_gaming_root_arg,
    env_path,
    eprint,
    ensure_scripts_on_path,
    resolve_gaming_path,
)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="Reference implementation: scripts/launch-srm.sh",
    )
    add_gaming_root_arg(parser)
    parser.add_argument(
        "--sandbox",
        action="store_true",
        help="enable Electron sandbox (default: --no-sandbox)",
    )
    parser.add_argument(
        "--appimage",
        type=Path,
        default=env_path("ARCHSTREAMER_SRM_APPIMAGE"),
        help="SRM AppImage (default: <gaming-root>/tools/srm/Steam-ROM-Manager.AppImage)",
    )
    parser.add_argument(
        "--art-root",
        type=Path,
        default=env_path("ARCHSTREAMER_ART_ROOT"),
        help="Art root (default: <gaming-root>/ROMS/Art)",
    )
    parser.add_argument(
        "--roms-root",
        type=Path,
        default=env_path("ARCHSTREAMER_ROMS_ROOT"),
        help="ROMs root (default: <gaming-root>/ROMS/Games)",
    )
    args, passthrough = parser.parse_known_args(argv)

    appimage = resolve_gaming_path(
        gaming_root=args.gaming_root,
        relative=REL_SRM_APPIMAGE,
        override=args.appimage,
        label="SRM AppImage (--appimage)",
    )
    art_root = resolve_gaming_path(
        gaming_root=args.gaming_root,
        relative=REL_ART_ROOT,
        override=args.art_root,
        label="art root (--art-root)",
    )
    roms_root = resolve_gaming_path(
        gaming_root=args.gaming_root,
        relative=REL_ROM_ROOT,
        override=args.roms_root,
        label="ROMs root (--roms-root)",
    )

    if not (appimage.is_file() and os.access(appimage, os.X_OK)):
        eprint(f"Steam ROM Manager AppImage not found at: {appimage}")
        eprint("Download it with: python3 scripts/install_srm.py --gaming-root <path>")
        return 1

    for sub in ("default", "poster", "heroes", "logos", "icons", "grids"):
        (art_root / sub).mkdir(parents=True, exist_ok=True)

    ensure_scripts_on_path()
    from ensure_srm_steam_layout import ensure_layout

    ensure_layout()

    print(f"Steam ROM Manager: {appimage}")
    print(f"ROMs root:         {roms_root}")
    print(f"Art root:          {art_root}")
    print()
    print("In SRM Settings → Environment Variables, set:")
    print(f"  ROMs Directory:           {roms_root}")
    print(f"  Local Images Directory:   {art_root}")
    print()
    print("Recommended Local Artwork globs (per parser):")
    print("  poster: ${localImagesDir}/poster/${title}.@(png|jpg|jpeg|webp)")
    print("  hero:   ${localImagesDir}/heroes/${title}.@(png|jpg|jpeg|webp)")
    print("  logo:   ${localImagesDir}/logos/${title}.@(png|jpg|jpeg|webp)")
    print("  icon:   ${localImagesDir}/icons/${title}.@(png|jpg|jpeg|webp)")
    print()
    print(
        "Enable 'DRM Protect' / artwork backup on parsers so SGDB choices are cached locally."
    )
    print("Then run: python3 scripts/sync_srm_art_into_catalog.py --gaming-root <path>")
    print()

    cmd = [str(appimage)]
    if not args.sandbox:
        cmd.append("--no-sandbox")
    cmd.extend(passthrough)
    os.execv(str(appimage), cmd)
    return 1  # pragma: no cover


if __name__ == "__main__":
    sys.exit(main())
