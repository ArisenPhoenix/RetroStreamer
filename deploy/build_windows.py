#!/usr/bin/env python3
"""Windows CMake build front-end (client by default; --build-host for host).

Reference implementation: build_windows.ps1 / build_windows.sh
See deploy/windows/README.md.
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

# Allow running from repo root or deploy/ without installing a package.
# scriptutil lives at <repo>/scripts/scriptutil.py (not deploy/scripts).
_ROOT = Path(__file__).resolve().parent.parent
_SCRIPTS = _ROOT / "scripts"
if str(_SCRIPTS) not in sys.path:
    sys.path.insert(0, str(_SCRIPTS))

from scriptutil import (  # noqa: E402
    default_vcpkg_root,
    ensure_msvc_on_path,
    eprint,
    repo_root,
    require_cmd,
    require_windows,
    run,
    which,
)


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(
        description="Configure and build ArchStreamer on Windows (vcpkg + CMake).",
        epilog="Reference implementation: build_windows.ps1 / build_windows.sh",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    p.add_argument("--reconfigure", action="store_true", help="Force cmake reconfigure")
    p.add_argument("--clean", action="store_true", help="Wipe build/ before configure")
    p.add_argument(
        "--install-deps",
        action="store_true",
        help="Run deploy/install_deps.py before building",
    )
    p.add_argument(
        "--build-host",
        action="store_true",
        help="Configure with -DARCHSTREAMER_BUILD_HOST=ON",
    )
    p.add_argument(
        "--vcpkg-root",
        type=Path,
        default=None,
        help="vcpkg root (default: VCPKG_ROOT or C:\\dev\\vcpkg)",
    )
    p.add_argument("--config", default="Release", help="CMake build config (default: Release)")
    p.add_argument(
        "--jobs",
        type=int,
        default=0,
        help="Parallel build jobs (default: CPU count, min 2)",
    )
    return p.parse_args()


def main() -> int:
    args = parse_args()
    require_windows()
    root = repo_root(Path(__file__))
    os.chdir(root)

    vcpkg = args.vcpkg_root or default_vcpkg_root()
    toolchain = vcpkg / "scripts" / "buildsystems" / "vcpkg.cmake"

    if args.install_deps:
        deps = root / "deploy" / "install_deps.py"
        run([sys.executable, deps, "--vcpkg-root", vcpkg], cwd=root)

    if not toolchain.is_file():
        eprint(f"vcpkg toolchain not found: {toolchain}")
        eprint("Set VCPKG_ROOT or pass --vcpkg-root.")
        return 1

    build_dir = root / "build"
    cache = build_dir / "CMakeCache.txt"

    if args.clean and build_dir.is_dir():
        print(f"Cleaning {build_dir} ...")
        shutil.rmtree(build_dir)

    host_flag = "ON" if args.build_host else "OFF"
    needs_configure = args.reconfigure or args.clean or not cache.is_file()

    # Ninja needs cl.exe on PATH. Developer PowerShell has it; GUI update often doesn't.
    msvc_ready = ensure_msvc_on_path()
    if not msvc_ready:
        eprint(
            "No C++ compiler on PATH (cl.exe). Install \"Desktop development with C++\" "
            "(Visual Studio or Build Tools), then retry."
        )

    if needs_configure:
        print(f"Configuring CMake (ARCHSTREAMER_BUILD_HOST={host_flag})...")
        require_cmd("cmake")
        cmake_cmd: list[str | Path] = ["cmake", "-S", root, "-B", build_dir]
        fresh_tree = args.clean or not cache.is_file()
        if fresh_tree and which("ninja") and msvc_ready:
            cmake_cmd.extend(["-G", "Ninja", f"-DCMAKE_BUILD_TYPE={args.config}"])
            print("Using Ninja generator.")
        elif fresh_tree and which("ninja") and not msvc_ready:
            print(
                "Ninja is installed but MSVC is not on PATH; "
                "using Visual Studio generator instead."
            )
        elif fresh_tree:
            print("Ninja not found; using CMake's default generator (often Visual Studio).")
        else:
            print("Reconfigure: keeping existing generator from build cache.")
        cmake_cmd.extend(
            [
                f"-DARCHSTREAMER_BUILD_HOST={host_flag}",
                f"-DCMAKE_TOOLCHAIN_FILE={toolchain}",
            ]
        )
        run(cmake_cmd, cwd=root)
    else:
        print(f"Reusing existing build cache ({cache}).")
        print("Pass --reconfigure to refresh cmake options, or --clean for a full rebuild.")
        if which("ninja") and not msvc_ready and cache.is_file():
            # Existing Ninja trees still need cl at build time.
            eprint(
                "Build cache looks like a Ninja tree but cl.exe is unavailable. "
                "Re-run with --clean so CMake can use the Visual Studio generator, "
                "or open \"x64 Native Tools Command Prompt for VS\" and rebuild."
            )

    jobs = args.jobs
    if jobs <= 0:
        jobs = max(2, os.cpu_count() or 4)

    print(f"Building ({args.config}, -j{jobs})...")
    require_cmd("cmake")
    run(
        ["cmake", "--build", build_dir, "--config", args.config, "--parallel", str(jobs)],
        cwd=root,
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except subprocess.CalledProcessError as exc:
        raise SystemExit(exc.returncode) from exc
