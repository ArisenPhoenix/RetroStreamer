#!/usr/bin/env python3
"""Download/update Steam ROM Manager AppImage."""

from __future__ import annotations

import argparse
import os
import stat
import sys
import urllib.request
from pathlib import Path

from scriptutil import (
    REL_SRM_DIR,
    add_gaming_root_arg,
    env_path,
    resolve_gaming_path,
)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="Reference implementation: scripts/install-srm.sh\n"
        "Default install dir under --gaming-root: tools/srm/",
    )
    add_gaming_root_arg(parser)
    parser.add_argument(
        "--dest-dir",
        type=Path,
        default=env_path("ARCHSTREAMER_SRM_DIR"),
        help="AppImage directory (default: <gaming-root>/tools/srm)",
    )
    parser.add_argument(
        "--version",
        default=os.environ.get("ARCHSTREAMER_SRM_VERSION", "v2.5.43"),
    )
    args = parser.parse_args(argv)

    dest_dir = resolve_gaming_path(
        gaming_root=args.gaming_root,
        relative=REL_SRM_DIR,
        override=args.dest_dir,
        label="SRM dest dir (--dest-dir)",
    )
    version: str = args.version
    dest = dest_dir / "Steam-ROM-Manager.AppImage"
    ver_num = version[1:] if version.startswith("v") else version
    url = (
        "https://github.com/SteamGridDB/steam-rom-manager/releases/download/"
        f"{version}/Steam-ROM-Manager-{ver_num}.AppImage"
    )

    dest_dir.mkdir(parents=True, exist_ok=True)
    print(f"Downloading {url}")
    partial = Path(str(dest) + ".partial")
    urllib.request.urlretrieve(url, partial)
    partial.replace(dest)
    if os.name != "nt":
        dest.chmod(dest.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
    size = dest.stat().st_size
    print(f"{dest}  {size} bytes")
    print("Launch with: python3 scripts/launch_srm.py --gaming-root <path>")
    return 0


if __name__ == "__main__":
    sys.exit(main())
