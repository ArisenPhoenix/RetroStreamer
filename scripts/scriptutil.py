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


def _vswhere_path() -> Path | None:
    base = os.environ.get("ProgramFiles(x86)") or r"C:\Program Files (x86)"
    candidate = Path(base) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
    return candidate if candidate.is_file() else None


def _vswhere_query(*extra: str) -> str:
    vswhere = _vswhere_path()
    if vswhere is None:
        return ""
    result = subprocess.run(
        [
            str(vswhere),
            "-latest",
            "-products",
            "*",
            "-requires",
            "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
            *extra,
        ],
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        check=False,
    )
    return (result.stdout or "").strip()


def find_vcvars64() -> Path | None:
    """Locate vcvars64.bat via vswhere (VS Build Tools / Visual Studio with C++)."""
    install = _vswhere_query("-property", "installationPath")
    if not install:
        return None
    bat = Path(install) / "VC" / "Auxiliary" / "Build" / "vcvars64.bat"
    return bat if bat.is_file() else None


def find_msvc_cl() -> Path | None:
    """Locate Hostx64\\x64\\cl.exe via vswhere -find (does not require Dev Shell)."""
    raw = _vswhere_query("-find", r"VC\Tools\MSVC\*\bin\Hostx64\x64\cl.exe")
    paths = [Path(line.strip()) for line in raw.splitlines() if line.strip()]
    paths = [p for p in paths if p.is_file()]
    if not paths:
        return None
    # Newest MSVC toolset first (path sorts by version string reasonably).
    paths.sort(key=lambda p: p.as_posix(), reverse=True)
    return paths[0]


def windows_vs_cmake_generator() -> list[str]:
    """Explicit VS generator args so cmake does not need cl.exe on PATH."""
    line = _vswhere_query("-property", "catalog_productLineVersion")
    # catalog_productLineVersion is like "2022" / "2019".
    mapping = {
        "2022": "Visual Studio 17 2022",
        "2019": "Visual Studio 16 2019",
        "2017": "Visual Studio 15 2017",
    }
    name = mapping.get(line.strip(), "Visual Studio 17 2022")
    return ["-G", name, "-A", "x64"]


def _decode_cmd_output(data: bytes | str) -> str:
    if isinstance(data, str):
        return data
    for enc in ("mbcs", "oem", "utf-8"):
        try:
            return data.decode(enc)
        except UnicodeDecodeError:
            continue
    return data.decode("utf-8", errors="replace")


def _apply_vcvars_env(vcvars: Path) -> bool:
    """Run vcvars64.bat and merge its environment into os.environ."""
    # cmd.exe needs this quote pattern for bat paths that contain spaces.
    command = f'"{vcvars}" && set'
    result = subprocess.run(
        ["cmd.exe", "/c", command],
        capture_output=True,
        check=False,
    )
    if result.returncode != 0:
        err = _decode_cmd_output(result.stderr or result.stdout or b"vcvars64.bat failed")
        eprint(err.strip())
        return False
    text = _decode_cmd_output(result.stdout or b"")
    applied = 0
    for line in text.splitlines():
        if "=" not in line:
            continue
        key, _, value = line.partition("=")
        if not key or key.startswith("!") or key.lower() in {"pwd", "cd"}:
            continue
        os.environ[key] = value
        applied += 1
    return applied > 0 and bool(which("cl") or which("clang-cl"))


def ensure_msvc_on_path() -> bool:
    """Ensure cl.exe (+ SDK env) is available for Ninja builds.

    GUI self-update and normal PowerShell often lack the VS Developer environment.
    """
    if which("cl") or which("clang-cl"):
        return True
    if sys.platform != "win32":
        return False

    # Prefer full vcvars (sets INCLUDE/LIB/PATH). Fall back to putting cl on PATH.
    vcvars = find_vcvars64()
    if vcvars is not None and _apply_vcvars_env(vcvars):
        return True

    cl = find_msvc_cl()
    if cl is not None:
        cl_dir = str(cl.parent)
        path = os.environ.get("PATH", "")
        if cl_dir.lower() not in path.lower():
            os.environ["PATH"] = cl_dir + os.pathsep + path
        if which("cl"):
            eprint(
                f"Found cl.exe at {cl} but full VS env (vcvars) did not load; "
                "Ninja builds may still fail — prefer the Visual Studio CMake generator."
            )
            return True
    return False


def cmake_cache_generator(cache_file: Path) -> str:
    """Read CMAKE_GENERATOR from an existing CMakeCache.txt (empty if missing)."""
    if not cache_file.is_file():
        return ""
    try:
        for line in cache_file.read_text(encoding="utf-8", errors="replace").splitlines():
            if line.startswith("CMAKE_GENERATOR:"):
                _, _, value = line.partition("=")
                return value.strip()
    except OSError:
        return ""
    return ""


def default_vcpkg_root() -> Path:
    env = os.environ.get("VCPKG_ROOT", "").strip()
    if env:
        return Path(env)
    if sys.platform == "win32":
        return Path(r"C:\dev\vcpkg")
    return Path("/c/dev/vcpkg")
