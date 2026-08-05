#!/usr/bin/env python3
"""Shared helpers for ArchStreamer deploy/build Python front-ends."""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Sequence

# Layout under a user-chosen Gaming root (--gaming-root / ARCHSTREAMER_GAMING_ROOT).
REL_ROM_ROOT = "ROMS/Games"
REL_META_ROOT = "ROMS/Meta"
REL_ART_ROOT = "ROMS/Art"
REL_DLC_ROOT = "ROMS/DLC"
REL_BIOS_ROOT = "BIOS FILES"
REL_SRM_DIR = "tools/srm"
REL_SRM_APPIMAGE = "tools/srm/Steam-ROM-Manager.AppImage"


def eprint(*args: object, **kwargs: object) -> None:
    print(*args, file=sys.stderr, **kwargs)


def env_path(name: str) -> Path | None:
    value = os.environ.get(name, "").strip()
    return Path(value) if value else None


def add_gaming_root_arg(parser) -> None:
    parser.add_argument(
        "--gaming-root",
        type=Path,
        default=env_path("ARCHSTREAMER_GAMING_ROOT"),
        help="Gaming tree root (contains ROMS/, BIOS FILES/, tools/). "
        "Env: ARCHSTREAMER_GAMING_ROOT",
    )


def resolve_gaming_path(
    *,
    gaming_root: Path | None,
    relative: str,
    override: Path | None,
    label: str,
) -> Path:
    """Resolve a path under the Gaming tree; *override* wins over gaming-root."""
    if override is not None:
        return override
    if gaming_root is not None:
        return gaming_root / relative
    raise SystemExit(
        f"Provide --gaming-root (or ARCHSTREAMER_GAMING_ROOT), or an explicit path for {label}"
    )


def repo_root(start: Path | None = None) -> Path:
    """Walk upward from *start* (or this file) until CMakeLists.txt is found."""
    cur = (start or Path(__file__)).resolve()
    if cur.is_file():
        cur = cur.parent
    for candidate in [cur, *cur.parents]:
        if (candidate / "CMakeLists.txt").is_file():
            return candidate
    raise FileNotFoundError(
        f"Could not find ArchStreamer repo root (CMakeLists.txt) from {start or Path(__file__)}"
    )


def ensure_scripts_on_path() -> Path:
    """Insert scripts/ onto sys.path and return the repo root."""
    root = repo_root()
    scripts = root / "scripts"
    scripts_str = str(scripts)
    if scripts_str not in sys.path:
        sys.path.insert(0, scripts_str)
    return root


def which(name: str) -> str | None:
    return shutil.which(name)


def require_cmd(name: str) -> str:
    path = which(name)
    if path is None:
        raise SystemExit(f"Required command not found on PATH: {name}")
    return path


def run(
    argv: Sequence[str | Path],
    *,
    check: bool = True,
    cwd: Path | None = None,
    env: dict[str, str] | None = None,
) -> subprocess.CompletedProcess[str]:
    cmd = [str(a) for a in argv]
    print("+", " ".join(cmd), flush=True)
    return subprocess.run(
        cmd,
        check=check,
        cwd=str(cwd) if cwd is not None else None,
        env=env,
        text=True,
    )


def require_windows() -> None:
    if sys.platform != "win32":
        raise SystemExit("This script is Windows-only.")


def require_linux() -> None:
    if sys.platform.startswith("win"):
        raise SystemExit("This script is intended for Linux/Unix hosts.")


def default_vcpkg_root() -> Path:
    env = os.environ.get("VCPKG_ROOT", "").strip()
    if env:
        return Path(env)
    if sys.platform == "win32":
        return Path(r"C:\dev\vcpkg")
    return Path("/c/dev/vcpkg")
