#!/usr/bin/env python3
"""Install ArchStreamer .desktop + hicolor icons for the current user.

Reference implementation: deploy/linux/install-user-branding.sh
"""

from __future__ import annotations

import argparse
import os
import shutil
import stat
import subprocess
import sys
from pathlib import Path

_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(_ROOT / "scripts"))
from scriptutil import (  # noqa: E402
    eprint,
    repo_root,
    require_linux,
    which,
)


APP_ID = "io.github.ArisenPhoenix.ArchStreamer"
ICON_NAME = APP_ID


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(
        description=(
            "Install ArchStreamer .desktop + hicolor icons under XDG_DATA_HOME "
            "so build-tree / terminal runs get branding under GNOME/KDE/Wayland."
        ),
        epilog="Reference implementation: deploy/linux/install-user-branding.sh",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    p.add_argument(
        "binary",
        nargs="?",
        type=Path,
        default=None,
        help="Path to archstreamer_gui (default: build/archstreamer_gui or PATH)",
    )
    return p.parse_args()


def _resolve_binary(root: Path, explicit: Path | None) -> Path:
    if explicit is not None:
        bin_path = explicit.expanduser()
        if not bin_path.is_file():
            raise SystemExit(f"Binary not found: {bin_path}")
        return bin_path.resolve()

    build_bin = root / "build" / "archstreamer_gui"
    if build_bin.is_file() and os.access(build_bin, os.X_OK):
        return build_bin.resolve()

    found = which("archstreamer_gui")
    if found:
        return Path(found).resolve()

    eprint(f"usage: {Path(sys.argv[0]).name} /path/to/archstreamer_gui")
    raise SystemExit(1)


def _install_file(src: Path, dest: Path, mode: int = 0o644) -> None:
    dest.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src, dest)
    dest.chmod(mode)


def main() -> int:
    args = parse_args()
    require_linux()
    root = repo_root(Path(__file__))
    branding = root / "branding"
    binary = _resolve_binary(root, args.binary)

    xdg_data = Path(
        os.environ.get("XDG_DATA_HOME")
        or (Path.home() / ".local" / "share")
    )
    app_dir = xdg_data / "applications"
    icon_root = xdg_data / "icons" / "hicolor"

    app_dir.mkdir(parents=True, exist_ok=True)
    (icon_root / "scalable" / "apps").mkdir(parents=True, exist_ok=True)

    for size in (128, 256, 512):
        src = branding / f"archstreamer-icon-{size}.png"
        if not src.is_file():
            eprint(f"Warning: missing icon asset {src}")
            continue
        _install_file(
            src,
            icon_root / f"{size}x{size}" / "apps" / f"{ICON_NAME}.png",
        )

    svg = branding / "archstreamer-icon.svg"
    if svg.is_file():
        _install_file(svg, icon_root / "scalable" / "apps" / f"{ICON_NAME}.svg")
    else:
        eprint(f"Warning: missing icon asset {svg}")

    desktop_path = app_dir / f"{APP_ID}.desktop"
    desktop_body = (
        "[Desktop Entry]\n"
        "Type=Application\n"
        "Name=ArchStreamer\n"
        "Comment=Local/LAN RetroArch streaming host and client\n"
        f"Exec={binary}\n"
        f"Icon={ICON_NAME}\n"
        "Terminal=false\n"
        "Categories=Game;Emulator;\n"
        "StartupWMClass=ArchStreamer\n"
    )
    desktop_path.write_text(desktop_body, encoding="utf-8")
    desktop_path.chmod(
        desktop_path.stat().st_mode | stat.S_IRUSR | stat.S_IWUSR | stat.S_IRGRP | stat.S_IROTH
    )

    if which("update-desktop-database"):
        subprocess.run(
            ["update-desktop-database", str(app_dir)],
            check=False,
            capture_output=True,
        )
    if which("gtk-update-icon-cache"):
        subprocess.run(
            ["gtk-update-icon-cache", "-f", "-t", str(icon_root)],
            check=False,
            capture_output=True,
        )

    print(f"Installed user desktop entry: {desktop_path}")
    print(f"  Exec={binary}")
    print(f"  Icons under {icon_root}")
    print("Re-launch ArchStreamer (quit fully first) to pick up the icon.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
