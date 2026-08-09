#!/usr/bin/env python3
"""Pull latest from GitHub, build ArchStreamer, and install it.

Reference implementation: deploy/windows/update-and-install.ps1
"""

from __future__ import annotations

import argparse
import os
import shlex
import shutil
import subprocess
import sys
import tempfile
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
)

APP_ID = "io.github.ArisenPhoenix.ArchStreamer"
ICON_NAME = APP_ID


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(
        description=(
            "Pull latest from GitHub, build ArchStreamer, and install it."
        ),
        epilog=(
            "Windows default prefix: C:\\Program Files\\ArchStreamer\n"
            "Linux default prefix:   /srv/Gaming/ArchStreamer when /srv/Gaming exists, "
            "otherwise ~/.local"
        ),
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
        help="Host-capable GUI (Windows: ViGEm; Linux: native host runtime)",
    )
    p.add_argument(
        "--no-host",
        action="store_true",
        help="Refrain from building the host"
    )
    p.add_argument("--reconfigure", action="store_true", help="Force cmake reconfigure")
    p.add_argument("--clean", action="store_true", help="Wipe build/ first")
    p.add_argument("--launch", action="store_true", help="Start the installed GUI when done")
    p.add_argument(
        "--prefix",
        type=Path,
        default=None,
        help="Install root (default depends on platform)",
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
        default=None,
        help="Git branch to pull (default: current branch, or master if detached)",
    )
    p.add_argument(
        "--desktop-scope",
        choices=("user", "system", "none"),
        default="user",
        help=(
            "Linux launcher install scope: user requires no sudo, system is "
            "visible to all users, none skips launcher registration (default: user)"
        ),
    )
    return p.parse_args()


def _current_git_branch(root: Path) -> str:
    result = subprocess.run(
        ["git", "rev-parse", "--abbrev-ref", "HEAD"],
        cwd=str(root),
        capture_output=True,
        text=True,
        check=False,
    )
    branch = (result.stdout or "").strip()
    if result.returncode == 0 and branch and branch != "HEAD":
        return branch
    return "master"


def _default_prefix() -> Path:
    if sys.platform == "win32":
        return Path(r"C:\Program Files\ArchStreamer")
    if sys.platform.startswith("linux"):
        gaming = Path("/srv/Gaming")
        if gaming.is_dir():
            return gaming / "ArchStreamer"
        return Path.home() / ".local"
    raise SystemExit(f"Unsupported platform: {sys.platform}")


def _stop_archstreamer_procs_windows() -> None:
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
    ]
    for name in names:
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


def _stop_archstreamer_procs_linux() -> None:
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
        # Linux comm names are capped at 15 bytes, so pkill -x misses
        # archstreamer_gui. Match argv instead and require a path/basename edge.
        pattern = rf"(^|/){name}([[:space:]]|$)"
        subprocess.run(
            ["pkill", "-f", pattern],
            capture_output=True,
            text=True,
            check=False,
        )


def _stop_archstreamer_procs() -> None:
    if sys.platform == "win32":
        _stop_archstreamer_procs_windows()
    else:
        _stop_archstreamer_procs_linux()


def _pull_latest(root: Path, branch: str, *, reset_hard: bool) -> None:
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
    print(f"Git: {head.stdout.strip()} {subject.stdout.strip()} [{branch}]")


def _build_windows(root: Path, args: argparse.Namespace, jobs: int) -> None:
    # Prefer deploy/ (current layout); fall back to repo-root copies from older trees.
    build_py = root / "deploy" / "build_windows.py"
    if not build_py.is_file():
        build_py = root / "build_windows.py"
    if not build_py.is_file():
        raise SystemExit(
            f"Could not find build_windows.py under deploy/ or repo root: {root}"
        )

    vcpkg_root = Path(args.vcpkg_root) if args.vcpkg_root else default_vcpkg_root()
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
    if args.no_host:
        build_cmd.append("--no-host")
    elif args.build_host:
        build_cmd.append("--build-host")
        
    if args.reconfigure:
        build_cmd.append("--reconfigure")
    if args.clean:
        build_cmd.append("--clean")
    run(build_cmd, cwd=root)


def _configure_and_build_linux(root: Path, args: argparse.Namespace, jobs: int) -> None:
    require_cmd("cmake")
    build_dir = root / "build"
    if args.clean and build_dir.exists():
        print(f"Removing {build_dir} ...")
        shutil.rmtree(build_dir)

    cache = build_dir / "CMakeCache.txt"
    need_configure = args.clean or args.reconfigure or not cache.is_file()
    build_dir.mkdir(parents=True, exist_ok=True)

    if need_configure:
        cmake_cmd: list[str | Path] = [
            "cmake",
            "-S",
            root,
            "-B",
            build_dir,
            f"-DCMAKE_BUILD_TYPE={args.config}",
        ]
        if args.no_host:
            cmake_cmd.append("-DARCHSTREAMER_BUILD_HOST=OFF")
        elif args.build_host:
            cmake_cmd.append("-DARCHSTREAMER_BUILD_HOST=ON")
        run(cmake_cmd, cwd=root)
    else:
        print("Reusing existing CMake configuration.")

    run(
        [
            "cmake",
            "--build",
            build_dir,
            "--config",
            args.config,
            "-j",
            str(jobs),
        ],
        cwd=root,
    )


def _install_windows(root: Path, args: argparse.Namespace, prefix: Path) -> None:
    print(f"Installing to {prefix} ...", flush=True)
    install_bin = prefix / "bin"
    vcpkg_root = Path(args.vcpkg_root) if args.vcpkg_root else default_vcpkg_root()
    _stop_archstreamer_procs()
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
        eprint(
            f"cmake --install failed (attempt {attempt}/5). "
            "Retrying after stopping processes again..."
        )
        _stop_archstreamer_procs()
        time.sleep(1)

    if not install_ok:
        raise SystemExit(
            f"cmake --install failed (often permission denied on {install_bin}\\*.exe).\n"
            "\n"
            "Common causes:\n"
            "  1. ArchStreamer / session_client still running — close them (Task Manager).\n"
            "  2. Not elevated — Program Files needs Admin PowerShell.\n"
            "  3. Antivirus briefly locking the new binaries — retry.\n"
            "\n"
            "Then re-run:\n"
            "  python deploy/update_and_install.py --skip-pull"
        )

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


def _install_linux(root: Path, args: argparse.Namespace, prefix: Path) -> None:
    print(f"Installing to {prefix} ...", flush=True)
    _stop_archstreamer_procs()
    time.sleep(0.5)

    require_cmd("cmake")
    install_ok = False
    for attempt in range(1, 4):
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
            f"cmake --install failed (attempt {attempt}/3). "
            "Retrying after stopping processes again..."
        )
        _stop_archstreamer_procs()
        time.sleep(1)

    if not install_ok:
        raise SystemExit(
            f"cmake --install failed for {prefix}.\n"
            "\n"
            "Common causes:\n"
            "  1. ArchStreamer / session_client / host_runner still running.\n"
            "  2. Prefix is not writable. Use --prefix under a writable directory "
            "or run with sudo.\n"
            "  3. Missing Linux build dependencies; rerun with --reconfigure after "
            "installing the package named by CMake.\n"
            "\n"
            "Then re-run:\n"
            "  python3 deploy/update_and_install.py --skip-pull"
        )

    gui = prefix / "bin" / "archstreamer_gui"
    _install_linux_desktop_entry(root, prefix, gui, args.desktop_scope)
    print("")
    print("Done. Installed ArchStreamer for Linux.")
    print("Run:")
    print(f'  "{gui}"')
    if args.launch:
        print("Launching installed GUI...")
        subprocess.Popen([str(gui)], cwd=str(root), start_new_session=True)


def _install_file(src: Path, dest: Path, mode: str = "0644") -> None:
    dest.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src, dest)
    dest.chmod(int(mode, 8))


def _install_files_privileged(
    files: list[tuple[Path, Path, str]],
    update_cmds: list[list[str | Path]],
) -> None:
    if not files and not update_cmds:
        return
    if os.geteuid() == 0:
        for src, dest, mode in files:
            run(["install", "-D", "-m", mode, src, dest])
        for cmd in update_cmds:
            subprocess.run([str(a) for a in cmd], check=False)
        return

    lines = ["set -eu"]
    for src, dest, mode in files:
        lines.append(
            "install -D -m "
            f"{shlex.quote(mode)} {shlex.quote(str(src))} {shlex.quote(str(dest))}"
        )
    for cmd in update_cmds:
        lines.append(" ".join(shlex.quote(str(a)) for a in cmd) + " || true")

    with tempfile.NamedTemporaryFile(
        "w",
        encoding="utf-8",
        prefix="archstreamer-install-",
        suffix=".sh",
        delete=False,
    ) as tmp:
        tmp.write("\n".join(lines))
        tmp.write("\n")
        script = Path(tmp.name)
    try:
        script.chmod(0o700)
        run(_privileged_command(["sh", script]))
    finally:
        script.unlink(missing_ok=True)


def _privileged_command(argv: list[str | Path]) -> list[str | Path]:
    if os.geteuid() == 0:
        return argv
    if sys.platform.startswith("linux") and shutil.which("pkexec") and (
        os.environ.get("DISPLAY") or os.environ.get("WAYLAND_DISPLAY")
    ):
        return ["pkexec", *argv]
    require_cmd("sudo")
    return ["sudo", *argv]


def _run_update(argv: list[str | Path]) -> None:
    if not argv:
        return
    subprocess.run([str(a) for a in argv], check=False)


def _install_linux_desktop_entry(
    root: Path,
    prefix: Path,
    gui: Path,
    desktop_scope: str,
) -> None:
    if desktop_scope == "none":
        print("Skipping Linux desktop entry registration.")
        return
    if not gui.is_file():
        eprint(f"Warning: GUI binary missing; skipping desktop entry: {gui}")
        return

    if desktop_scope == "system":
        app_dir = Path("/usr/local/share/applications")
        icon_root = Path("/usr/local/share/icons/hicolor")
    else:
        user_data = Path(os.environ.get("XDG_DATA_HOME", Path.home() / ".local" / "share"))
        app_dir = user_data / "applications"
        icon_root = user_data / "icons" / "hicolor"

    installed_icon_root = prefix / "share" / "icons" / "hicolor"
    branding = root / "branding"

    desktop_body = (
        "[Desktop Entry]\n"
        "Type=Application\n"
        "Name=ArchStreamer\n"
        "Comment=Local/LAN RetroArch streaming host and client\n"
        f"Exec={gui}\n"
        f"TryExec={gui}\n"
        f"Icon={ICON_NAME}\n"
        "Terminal=false\n"
        "Categories=Game;Emulator;\n"
        "StartupWMClass=ArchStreamer\n"
    )
    with tempfile.NamedTemporaryFile(
        "w",
        encoding="utf-8",
        prefix="archstreamer-",
        suffix=".desktop",
        delete=False,
    ) as tmp:
        tmp.write(desktop_body)
        desktop_tmp = Path(tmp.name)
    install_files = []
    try:
        desktop_dest = app_dir / f"{APP_ID}.desktop"
        install_files.append((desktop_tmp, desktop_dest, "0644"))

        for size in (128, 256, 512):
            rel = Path(f"{size}x{size}") / "apps" / f"{ICON_NAME}.png"
            src = installed_icon_root / rel
            if not src.is_file():
                src = branding / f"archstreamer-icon-{size}.png"
            if src.is_file():
                install_files.append((src, icon_root / rel, "0644"))
            else:
                eprint(f"Warning: missing icon asset for {size}x{size}")

        svg_rel = Path("scalable") / "apps" / f"{ICON_NAME}.svg"
        svg_src = installed_icon_root / svg_rel
        if not svg_src.is_file():
            svg_src = branding / "archstreamer-icon.svg"
        if svg_src.is_file():
            install_files.append((svg_src, icon_root / svg_rel, "0644"))
        else:
            eprint("Warning: missing SVG icon asset")

        update_cmds = []
        if shutil.which("update-desktop-database"):
            update_cmds.append(["update-desktop-database", app_dir])
        if shutil.which("gtk-update-icon-cache"):
            update_cmds.append(["gtk-update-icon-cache", "-f", "-t", icon_root])

        if desktop_scope == "system":
            _install_files_privileged(install_files, update_cmds)
        else:
            for src, dest, mode in install_files:
                _install_file(src, dest, mode)
            for cmd in update_cmds:
                _run_update(cmd)
    finally:
        desktop_tmp.unlink(missing_ok=True)

    print(f"Installed desktop entry: {desktop_dest}")
    print(f"Installed icons under: {icon_root}")


def main() -> int:
    args = parse_args()
    if sys.platform != "win32" and not sys.platform.startswith("linux"):
        raise SystemExit(f"Unsupported platform: {sys.platform}")

    root = repo_root(Path(__file__))
    prefix = Path(args.prefix) if args.prefix is not None else _default_prefix()
    prefix = prefix.expanduser()
    branch = (args.branch or _current_git_branch(root)).strip()
    if not branch:
        raise SystemExit("--branch must not be empty (default is master)")

    os.chdir(root)
    platform_label = "Windows" if sys.platform == "win32" else "Linux"
    print(f"=== ArchStreamer {platform_label} update ===")
    print(f"Repo: {root}")
    print(f"Branch: {branch}")
    print(f"Install prefix: {prefix}")

    if not args.skip_pull:
        _pull_latest(root, branch, reset_hard=args.reset_hard)
    else:
        print("Skipping git pull.")

    jobs = args.jobs if args.jobs > 0 else (os.cpu_count() or 2)
    print(f"Building (-j{jobs})...")
    if sys.platform == "win32":
        _build_windows(root, args, jobs)
        if args.skip_install:
            print("Skipping install. Binary under build\\ or build\\Release\\")
            return 0
        _install_windows(root, args, prefix)
    else:
        _configure_and_build_linux(root, args, jobs)
        if args.skip_install:
            print("Skipping install. Binaries are under build/")
            return 0
        _install_linux(root, args, prefix)

    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except subprocess.CalledProcessError as exc:
        raise SystemExit(exc.returncode) from exc
