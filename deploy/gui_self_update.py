#!/usr/bin/env python3
"""GUI self-update helper: check how far behind origin/<branch> is, or apply an update.

Used by the desktop Settings → Updates controls (non-Flatpak).

Examples:
  python deploy/gui_self_update.py check --branch master
  python deploy/gui_self_update.py apply --branch master --reset-hard --launch
"""

from __future__ import annotations

import argparse
import os
import shutil
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
    run,
    which,
)


def _git(root: Path, *args: str, check: bool = True) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        ["git", *args],
        cwd=str(root),
        check=check,
        capture_output=True,
        text=True,
    )


def _short(root: Path, ref: str) -> str:
    out = _git(root, "rev-parse", "--short", ref, check=False)
    if out.returncode != 0:
        return ""
    return out.stdout.strip()


def cmd_check(root: Path, branch: str) -> int:
    require_cmd("git")
    remote_ref = f"origin/{branch}"
    print(f"repo={root}")
    print(f"branch={branch}")

    fetch = _git(root, "fetch", "origin", branch, check=False)
    if fetch.returncode != 0:
        # Broader fetch if the named branch ref fetch fails.
        fetch = _git(root, "fetch", "origin", check=False)
    if fetch.returncode != 0:
        eprint(fetch.stderr.strip() or "git fetch failed")
        print("status=fetch_failed")
        return 2

    verify = _git(root, "rev-parse", "--verify", remote_ref, check=False)
    if verify.returncode != 0:
        print(f"status=missing_remote_branch")
        print(f"remote_ref={remote_ref}")
        return 3

    local = _short(root, "HEAD")
    remote = _short(root, remote_ref)
    behind = _git(root, "rev-list", "--count", f"HEAD..{remote_ref}", check=False)
    ahead = _git(root, "rev-list", "--count", f"{remote_ref}..HEAD", check=False)
    dirty = _git(root, "status", "--porcelain", check=False)
    subject = _git(root, "log", "-1", "--pretty=%s", check=False)

    behind_n = int(behind.stdout.strip() or "0") if behind.returncode == 0 else -1
    ahead_n = int(ahead.stdout.strip() or "0") if ahead.returncode == 0 else -1
    is_dirty = bool((dirty.stdout or "").strip()) if dirty.returncode == 0 else False

    print(f"local={local}")
    print(f"remote={remote}")
    print(f"behind={behind_n}")
    print(f"ahead={ahead_n}")
    print(f"dirty={'1' if is_dirty else '0'}")
    if subject.returncode == 0:
        print(f"subject={subject.stdout.strip()}")

    if behind_n < 0:
        print("status=error")
        return 4
    if behind_n == 0 and ahead_n == 0:
        print("status=up_to_date")
    elif behind_n > 0:
        print("status=update_available")
    else:
        print("status=ahead_of_remote")
    return 0


def _stop_procs_windows() -> None:
    names = [
        "archstreamer_gui",
        "session_client",
        "host_runner",
        "client_catalog_probe",
        "game_catalog_probe",
        "asset_probe",
        "steam_art_import",
        "uinput_probe",
        "controller_probe",
        "archstreamer_ssh_askpass",
    ]
    for name in names:
        subprocess.run(
            ["taskkill", "/F", "/IM", f"{name}.exe"],
            capture_output=True,
            text=True,
            check=False,
        )


def _stop_procs_linux() -> None:
    names = [
        "archstreamer_gui",
        "host_runner",
        "session_client",
    ]
    for name in names:
        subprocess.run(["pkill", "-x", name], capture_output=True, text=True, check=False)


def _pull(root: Path, branch: str, reset_hard: bool) -> None:
    remote_ref = f"origin/{branch}"
    run(["git", "fetch", "origin"], cwd=root)
    verify = _git(root, "rev-parse", "--verify", remote_ref, check=False)
    if verify.returncode != 0:
        raise SystemExit(f"Remote branch not found: {remote_ref}")

    dirty = bool(_git(root, "status", "--porcelain").stdout.strip())
    if reset_hard:
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
        raise SystemExit(
            "Working tree has local changes. Re-run with --reset-hard, "
            "or commit/stash locally."
        )
    else:
        checkout = subprocess.run(
            ["git", "checkout", branch],
            cwd=str(root),
            check=False,
        )
        if checkout.returncode != 0:
            run(["git", "checkout", "-B", branch, remote_ref], cwd=root)
        run(["git", "pull", "--ff-only", "origin", branch], cwd=root)


def _build_windows(root: Path, args: argparse.Namespace) -> None:
    build_py = root / "deploy" / "build_windows.py"
    if not build_py.is_file():
        raise SystemExit(f"Missing {build_py}")
    vcpkg_root = Path(args.vcpkg_root) if args.vcpkg_root else default_vcpkg_root()
    cmd: list[str | Path] = [
        sys.executable,
        build_py,
        "--config",
        args.config,
        "--vcpkg-root",
        vcpkg_root,
    ]
    if args.build_host:
        cmd.append("--build-host")
    if args.reconfigure:
        cmd.append("--reconfigure")
    if args.clean:
        cmd.append("--clean")
    run(cmd, cwd=root)


def _install_windows(root: Path, args: argparse.Namespace) -> None:
    prefix = Path(args.prefix)
    vcpkg_root = Path(args.vcpkg_root) if args.vcpkg_root else default_vcpkg_root()
    print(f"Installing to {prefix} ...")
    _stop_procs_windows()
    time.sleep(0.5)
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
        eprint(f"cmake --install failed (attempt {attempt}/5); retrying...")
        _stop_procs_windows()
        time.sleep(1)
    if not install_ok:
        raise SystemExit(f"cmake --install failed for prefix {prefix}")

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
    if args.branch:
        finish_cmd.extend(["--gui-branch", args.branch])
    run(finish_cmd, cwd=root)


def _build_linux(root: Path, args: argparse.Namespace) -> Path:
    require_cmd("cmake")
    build_dir = root / "build"
    cache = build_dir / "CMakeCache.txt"
    need_configure = (not cache.is_file()) or args.reconfigure or args.clean
    if args.clean and build_dir.exists():
        shutil.rmtree(build_dir)
        need_configure = True
    build_dir.mkdir(parents=True, exist_ok=True)

    if need_configure:
        cmake_cmd = ["cmake", "-S", str(root), "-B", str(build_dir)]
        if args.build_host:
            cmake_cmd.append("-DARCHSTREAMER_BUILD_HOST=ON")
        run(cmake_cmd, cwd=root)

    targets = ["archstreamer_gui"]
    if args.build_host or (build_dir / "host_runner").exists():
        targets.append("host_runner")
    targets.append("archstreamer_ssh_askpass")

    jobs = os.cpu_count() or 4
    # askpass may be absent on very old trees; fall back to GUI-only.
    result = subprocess.run(
        ["cmake", "--build", str(build_dir), "-j", str(jobs), "--target", *targets],
        cwd=str(root),
        check=False,
        text=True,
    )
    if result.returncode != 0 and "archstreamer_ssh_askpass" in targets:
        eprint("Full target build failed; retrying without archstreamer_ssh_askpass...")
        targets = [t for t in targets if t != "archstreamer_ssh_askpass"]
        run(
            ["cmake", "--build", str(build_dir), "-j", str(jobs), "--target", *targets],
            cwd=root,
        )
    elif result.returncode != 0:
        raise SystemExit(result.returncode)

    gui = build_dir / "archstreamer_gui"
    if not gui.is_file():
        raise SystemExit(f"Build finished but GUI binary missing: {gui}")
    return gui


def cmd_apply(root: Path, args: argparse.Namespace) -> int:
    require_cmd("git")
    branch = (args.branch or "").strip()
    if not branch:
        raise SystemExit("--branch must not be empty")

    # Give the GUI a moment to exit after it spawned us detached.
    if args.wait_secs > 0:
        time.sleep(args.wait_secs)

    if sys.platform == "win32":
        _stop_procs_windows()
    else:
        _stop_procs_linux()
    time.sleep(0.3)

    os.chdir(root)
    print("=== ArchStreamer self-update ===")
    print(f"Repo: {root}")
    print(f"Branch: {branch}")
    print(f"Platform: {sys.platform}")

    if not args.skip_pull:
        _pull(root, branch, reset_hard=args.reset_hard)
        head = _short(root, "HEAD")
        subject = _git(root, "log", "-1", "--pretty=%s").stdout.strip()
        print(f"Git: {head} {subject} [{branch}]")
    else:
        print("Skipping git pull.")

    if sys.platform == "win32":
        _build_windows(root, args)
        if args.skip_install:
            print("Skipping install. Binary under build\\ or build\\Release\\")
            return 0
        _install_windows(root, args)
        print("Done.")
        return 0

    gui = _build_linux(root, args)
    print(f"Built: {gui}")
    if args.launch:
        cmd = [str(gui)]
        if branch:
            cmd.extend(["--branch", branch])
        print(f"Launching {' '.join(cmd)} ...")
        subprocess.Popen(
            cmd,
            cwd=str(root),
            start_new_session=True,
        )
    print("Done. Restart ArchStreamer if it was not relaunched.")
    return 0


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description="ArchStreamer GUI self-update helper")
    p.add_argument(
        "action",
        choices=("check", "apply"),
        help="check = report update status; apply = pull/build/install",
    )
    p.add_argument("--repo", type=Path, default=None, help="Repo root (default: auto)")
    p.add_argument("--branch", default="master", help="Git branch (default: master)")
    p.add_argument("--reset-hard", action="store_true", help="Discard local edits on apply")
    p.add_argument("--skip-pull", action="store_true")
    p.add_argument("--skip-install", action="store_true", help="Windows: build only")
    p.add_argument("--build-host", action="store_true")
    p.add_argument("--reconfigure", action="store_true")
    p.add_argument("--clean", action="store_true")
    p.add_argument("--launch", action="store_true", help="Relaunch GUI when finished")
    p.add_argument(
        "--wait-secs",
        type=float,
        default=1.5,
        help="Seconds to wait before apply (so the GUI can exit)",
    )
    p.add_argument(
        "--prefix",
        type=Path,
        default=Path(r"C:\Program Files\ArchStreamer"),
        help="Windows install prefix",
    )
    p.add_argument("--vcpkg-root", type=Path, default=None)
    p.add_argument("--config", default="Release")
    return p.parse_args()


def main() -> int:
    args = parse_args()
    root = Path(args.repo).resolve() if args.repo else repo_root(Path(__file__))
    if not (root / "CMakeLists.txt").is_file():
        raise SystemExit(f"Not an ArchStreamer repo root: {root}")
    if args.action == "check":
        return cmd_check(root, (args.branch or "master").strip())
    return cmd_apply(root, args)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except subprocess.CalledProcessError as exc:
        raise SystemExit(exc.returncode) from exc
