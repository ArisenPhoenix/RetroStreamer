#!/usr/bin/env python3
"""Pull latest from GitHub, build the Windows client, and install to Program Files.

Reference implementation: deploy/windows/update-and-install.ps1
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
import time
from pathlib import Path

_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(_ROOT / "scripts"))
from scriptutil import (  # noqa: E402
    default_vcpkg_root,
    eprint,
    repo_root,
    require_cmd,
    require_windows,
    run,
)


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(
        description=(
            "Pull latest from GitHub, build the Windows client, and install to Program Files."
        ),
        epilog="Reference implementation: deploy/windows/update-and-install.ps1",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    p.add_argument(
        "--reset-hard",
        action="store_true",
        help="Discard local edits and match origin/<branch>",
    )
    p.add_argument("--skip-pull", action="store_true", help="Build/install only (no git)")
    p.add_argument("--skip-install", action="store_true", help="Build only")
    p.add_argument(
        "--build-host",
        action="store_true",
        help="Host-capable GUI (needs ViGEm etc.)",
    )
    p.add_argument("--reconfigure", action="store_true", help="Force cmake reconfigure")
    p.add_argument("--clean", action="store_true", help="Wipe build/ first")
    p.add_argument("--launch", action="store_true", help="Start the installed GUI when done")
    p.add_argument(
        "--prefix",
        type=Path,
        default=Path(r"C:\Program Files\ArchStreamer"),
        help="Install root (default: C:\\Program Files\\ArchStreamer)",
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
        default=2,
        help="Parallel build jobs passed to cmake --build (default: 2)",
    )
    p.add_argument(
        "--branch",
        default="master",
        help="Git branch to pull (default: master)",
    )
    return p.parse_args()


_INSTALL_PROC_NAMES = (
    "archstreamer_gui",
    "session_client",
    "host_runner",
    "client_catalog_probe",
    "game_catalog_probe",
    "asset_probe",
    "steam_art_import",
    "uinput_probe",
    "controller_probe",
)


def _stop_archstreamer_procs() -> None:
    for name in _INSTALL_PROC_NAMES:
        subprocess.run(
            ["taskkill", "/F", "/IM", f"{name}.exe"],
            capture_output=True,
            text=True,
            check=False,
        )
        # Also try without .exe in case of short names via PowerShell.
        subprocess.run(
            [
                "powershell",
                "-NoProfile",
                "-Command",
                f"Get-Process -Name '{name}' -ErrorAction SilentlyContinue | "
                "Stop-Process -Force -ErrorAction SilentlyContinue",
            ],
            capture_output=True,
            text=True,
            check=False,
        )


def _move_locked_install_exes_aside(install_bin: Path) -> None:
    """Running images often cannot be overwritten but can be renamed aside."""
    if not install_bin.is_dir():
        return
    for name in _INSTALL_PROC_NAMES:
        exe = install_bin / f"{name}.exe"
        if not exe.is_file():
            continue
        bak = install_bin / f"{name}.exe.old"
        try:
            if bak.exists():
                bak.unlink()
        except OSError:
            pass
        try:
            exe.replace(bak)
            print(f"Moved locked/previous {name}.exe -> {name}.exe.old")
        except OSError as exc:
            eprint(f"Could not move {exe} aside: {exc}")


def _remove_obsolete_controller_probe(install_bin: Path) -> None:
    """controller_probe is no longer installed on Windows; clear leftovers."""
    obsolete = install_bin / "controller_probe.exe"
    if not obsolete.is_file():
        return
    try:
        obsolete.unlink()
        print(f"Removed obsolete {obsolete} (no longer installed on Windows).")
        return
    except OSError:
        pass
    bak = install_bin / "controller_probe.exe.old"
    try:
        if bak.exists():
            bak.unlink()
        obsolete.replace(bak)
        print("Moved obsolete controller_probe.exe aside (was locked).")
    except OSError as exc:
        eprint(f"Could not remove obsolete controller_probe.exe: {exc}")


def _list_locked_install_exes(install_bin: Path) -> list[Path]:
    locked: list[Path] = []
    if not install_bin.is_dir():
        return locked
    for name in _INSTALL_PROC_NAMES:
        exe = install_bin / f"{name}.exe"
        if not exe.is_file():
            continue
        try:
            with exe.open("r+b"):
                pass
        except OSError:
            locked.append(exe)
    return locked


def main() -> int:
    args = parse_args()
    require_windows()
    root = repo_root(Path(__file__))
    # Prefer deploy/ (current layout); fall back to repo-root copies from older trees.
    build_py = root / "deploy" / "build_windows.py"
    if not build_py.is_file():
        build_py = root / "build_windows.py"
    if not build_py.is_file():
        raise SystemExit(
            f"Could not find build_windows.py under deploy/ or repo root: {root}"
        )

    vcpkg_root = Path(args.vcpkg_root) if args.vcpkg_root else default_vcpkg_root()
    prefix = Path(args.prefix)
    branch = (args.branch or "").strip()
    if not branch:
        raise SystemExit("--branch must not be empty (default is master)")

    os.chdir(root)
    print("=== ArchStreamer Windows update ===")
    print(f"Repo: {root}")
    print(f"Branch: {branch}")

    if not args.skip_pull:
        require_cmd("git")
        print("Fetching origin...")
        run(["git", "fetch", "origin"], cwd=root)

        remote_ref = f"origin/{branch}"
        verify = subprocess.run(
            ["git", "rev-parse", "--verify", remote_ref],
            cwd=str(root),
            capture_output=True,
            text=True,
            check=False,
        )
        if verify.returncode != 0:
            raise SystemExit(
                f"Remote branch not found: {remote_ref} "
                "(push it first, or check --branch spelling)"
            )

        status = subprocess.run(
            ["git", "status", "--porcelain"],
            cwd=str(root),
            capture_output=True,
            text=True,
            check=True,
        )
        dirty = bool(status.stdout.strip())

        if args.reset_hard:
            print(f"Resetting to {remote_ref} (discarding local changes)...")
            checkout = subprocess.run(
                ["git", "checkout", branch],
                cwd=str(root),
                check=False,
            )
            if checkout.returncode != 0:
                run(["git", "checkout", "-B", branch, remote_ref], cwd=root)
            run(["git", "reset", "--hard", remote_ref], cwd=root)
            run(["git", "clean", "-fd"], cwd=root)
        elif dirty:
            print("Working tree has local changes:")
            run(["git", "status", "-sb"], cwd=root, check=False)
            raise SystemExit(
                "Refusing to pull over dirty tree. Re-run with --reset-hard, "
                "or commit/stash locally, or pass --skip-pull."
            )
        else:
            print(f"Pulling {remote_ref}...")
            checkout = subprocess.run(
                ["git", "checkout", branch],
                cwd=str(root),
                check=False,
            )
            if checkout.returncode != 0:
                run(["git", "checkout", "-B", branch, remote_ref], cwd=root)
            run(["git", "pull", "--ff-only", "origin", branch], cwd=root)

        head = subprocess.run(
            ["git", "rev-parse", "--short", "HEAD"],
            cwd=str(root),
            capture_output=True,
            text=True,
            check=True,
        )
        subject = subprocess.run(
            ["git", "log", "-1", "--pretty=%s"],
            cwd=str(root),
            capture_output=True,
            text=True,
            check=True,
        )
        print(
            f"Git: {head.stdout.strip()} {subject.stdout.strip()} [{branch}]"
        )
    else:
        print("Skipping git pull.")

    jobs = args.jobs if args.jobs > 0 else 2
    print(f"Building (-j{jobs})...")
    build_cmd: list[str | Path] = [
        sys.executable,
        build_py,
        "--config",
        args.config,
        "--vcpkg-root",
        vcpkg_root,
        "--jobs",
        str(jobs),
    ]
    if args.build_host:
        build_cmd.append("--build-host")
    if args.reconfigure:
        build_cmd.append("--reconfigure")
    if args.clean:
        build_cmd.append("--clean")
    run(build_cmd, cwd=root)

    if args.skip_install:
        print("Skipping install. Binary under build\\ or build\\Release\\")
        return 0

    print(f"Installing to {prefix} ...")
    install_bin = prefix / "bin"
    _stop_archstreamer_procs()
    time.sleep(0.5)
    _move_locked_install_exes_aside(install_bin)
    _remove_obsolete_controller_probe(install_bin)

    require_cmd("cmake")
    install_ok = False
    for attempt in range(1, 6):
        result = subprocess.run(
            [
                "cmake",
                "--install",
                "build",
                "--config",
                args.config,
                "--prefix",
                str(prefix),
            ],
            cwd=str(root),
            check=False,
        )
        if result.returncode == 0:
            install_ok = True
            break
        eprint(
            f"cmake --install failed (attempt {attempt}/5). "
            "Retrying after unlock..."
        )
        _stop_archstreamer_procs()
        time.sleep(1)
        _move_locked_install_exes_aside(install_bin)

    if not install_ok:
        locked = _list_locked_install_exes(install_bin)
        locked_msg = (
            "Still locked:\n  - " + "\n  - ".join(str(p) for p in locked)
            if locked
            else "No specific locked bin detected (may be Admin / AV)."
        )
        raise SystemExit(
            f"cmake --install failed (often permission denied on {install_bin}\\*.exe).\n"
            "\n"
            f"{locked_msg}\n"
            "\n"
            "Common causes:\n"
            "  1. ArchStreamer / session_client / controller_probe still running — "
            "close them (Task Manager).\n"
            "  2. Not elevated — Program Files needs Admin PowerShell.\n"
            "  3. Antivirus briefly locking the new binaries — retry.\n"
            "\n"
            "Then re-run:\n"
            "  python deploy/update_and_install.py --skip-pull"
        )

    for bak in install_bin.glob("*.exe.old") if install_bin.is_dir() else ():
        try:
            bak.unlink()
        except OSError:
            pass

    finish = root / "deploy" / "finish_install.py"
    finish_cmd: list[str | Path] = [
        sys.executable,
        finish,
        "--prefix",
        prefix,
        "--vcpkg-root",
        vcpkg_root,
        "--shortcuts",
    ]
    if args.launch:
        finish_cmd.append("--launch")
    run(finish_cmd, cwd=root)

    print("")
    print("Done. All users can launch ArchStreamer from the Start Menu")
    print("  (Programs → ArchStreamer) or Public Desktop.")
    print("Or run:")
    print(f'  & "{prefix}\\bin\\archstreamer_gui.exe"')
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except subprocess.CalledProcessError as exc:
        raise SystemExit(exc.returncode) from exc
