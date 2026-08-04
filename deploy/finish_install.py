#!/usr/bin/env python3
"""Finish a Program Files install after cmake --install.

Reference implementation: deploy/windows/finish-install.ps1
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(_ROOT / "scripts"))
from scriptutil import (  # noqa: E402
    default_vcpkg_root,
    eprint,
    require_windows,
    run,
)


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(
        description=(
            "Copy SDL2/Qt runtime deps next to the installed GUI and optionally "
            "create Start Menu / Desktop shortcuts."
        ),
        epilog="Reference implementation: deploy/windows/finish-install.ps1",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    p.add_argument(
        "--prefix",
        type=Path,
        default=Path(r"C:\Program Files\ArchStreamer"),
        help="Install prefix (default: C:\\Program Files\\ArchStreamer)",
    )
    p.add_argument(
        "--vcpkg-root",
        type=Path,
        default=None,
        help="vcpkg root (default: VCPKG_ROOT or C:\\dev\\vcpkg)",
    )
    p.add_argument(
        "--launch",
        action="store_true",
        help="Start archstreamer_gui.exe when finished",
    )
    p.add_argument(
        "--gui-branch",
        default=None,
        help="Pass --branch <name> to the GUI when launching (session-only)",
    )
    p.add_argument(
        "--shortcuts",
        action="store_true",
        help="Create Start Menu / Desktop .lnk shortcuts",
    )
    p.add_argument(
        "--current-user-only",
        action="store_true",
        help="With --shortcuts, install per-user shortcuts only (not All Users)",
    )
    return p.parse_args()


def _is_admin() -> bool:
    try:
        import ctypes

        return bool(ctypes.windll.shell32.IsUserAnAdmin())  # type: ignore[attr-defined]
    except Exception:
        return False


def _special_folder(name: str) -> Path | None:
    """Resolve a Windows special folder via PowerShell Environment.GetFolderPath."""
    try:
        proc = subprocess.run(
            [
                "powershell",
                "-NoProfile",
                "-Command",
                f"[Environment]::GetFolderPath('{name}')",
            ],
            capture_output=True,
            text=True,
            check=False,
        )
        out = (proc.stdout or "").strip()
        if proc.returncode == 0 and out:
            return Path(out)
    except OSError:
        pass
    return None


def _create_shortcut(
    link_path: Path,
    target_path: Path,
    working_directory: Path,
    description: str = "ArchStreamer",
) -> None:
    link_path.parent.mkdir(parents=True, exist_ok=True)
    icon = f"{target_path},0" if target_path.is_file() else ""

    # Prefer win32com if available.
    try:
        import win32com.client  # type: ignore[import-untyped]

        shell = win32com.client.Dispatch("WScript.Shell")
        shortcut = shell.CreateShortCut(str(link_path))
        shortcut.Targetpath = str(target_path)
        shortcut.WorkingDirectory = str(working_directory)
        shortcut.Description = description
        if icon:
            shortcut.IconLocation = icon
        shortcut.save()
        print(f"Shortcut: {link_path}")
        return
    except Exception:
        pass

    # Fallback: PowerShell WScript.Shell one-liner.
    icon_ps = (
        f"$lnk.IconLocation = '{icon}'"
        if icon
        else ""
    )
    ps = (
        f"$w = New-Object -ComObject WScript.Shell; "
        f"$lnk = $w.CreateShortcut('{link_path}'); "
        f"$lnk.TargetPath = '{target_path}'; "
        f"$lnk.WorkingDirectory = '{working_directory}'; "
        f"$lnk.Description = '{description}'; "
        f"{icon_ps}; "
        f"$lnk.Save()"
    )
    proc = subprocess.run(
        ["powershell", "-NoProfile", "-Command", ps],
        capture_output=True,
        text=True,
        check=False,
    )
    if proc.returncode != 0:
        msg = (proc.stderr or proc.stdout or "unknown error").strip()
        raise RuntimeError(msg)
    print(f"Shortcut: {link_path}")


def main() -> int:
    args = parse_args()
    require_windows()
    prefix = Path(args.prefix)
    vcpkg_root = Path(args.vcpkg_root) if args.vcpkg_root else default_vcpkg_root()
    cwd = Path.cwd()

    bin_dir = prefix / "bin"
    exe = bin_dir / "archstreamer_gui.exe"
    if not exe.is_file():
        raise SystemExit(
            f'Missing {exe} — run: cmake --install build --prefix "{prefix}"'
        )

    vcpkg_bin = vcpkg_root / "installed" / "x64-windows" / "bin"
    if not vcpkg_bin.is_dir():
        raise SystemExit(f"vcpkg bin not found: {vcpkg_bin} (pass --vcpkg-root)")

    # --- SDL2.dll (build tree first, then vcpkg) ---
    sdl_candidates = [
        cwd / "build" / "SDL2.dll",
        cwd / "build" / "Release" / "SDL2.dll",
        vcpkg_bin / "SDL2.dll",
    ]
    sdl_copied = False
    for src in sdl_candidates:
        if src.is_file():
            shutil.copy2(src, bin_dir / "SDL2.dll")
            print(f"Copied SDL2.dll from {src}")
            sdl_copied = True
            break
    if not sdl_copied:
        eprint(f"SDL2.dll not found. Controllers may fail until you copy it into {bin_dir}")

    # --- windeployqt ---
    deploy_candidates = [
        vcpkg_root / "installed" / "x64-windows" / "tools" / "Qt6" / "bin" / "windeployqt.exe",
        vcpkg_root / "installed" / "x64-windows" / "tools" / "Qt6" / "bin" / "windeployqt6.exe",
    ]
    found_deploy: Path | None = None
    for c in deploy_candidates:
        if c.is_file():
            found_deploy = c
            break
    if found_deploy is None and vcpkg_root.is_dir():
        for hit in vcpkg_root.rglob("windeployqt*.exe"):
            found_deploy = hit
            break
    if found_deploy is None:
        raise SystemExit(f"windeployqt.exe not found under {vcpkg_root}")

    print(f"Using windeployqt: {found_deploy}")
    run([found_deploy, "--release", exe])

    # windeployqt often skips Qt's vcpkg transitive deps; copy them explicitly.
    qt_deps = [
        "libpng16.dll",
        "harfbuzz.dll",
        "md4c.dll",
        "freetype.dll",
        "zlib1.dll",
        "double-conversion.dll",
        "pcre2-16.dll",
        "zstd.dll",
        "bz2.dll",
        "brotlidec.dll",
        "brotlicommon.dll",
        "brotlienc.dll",
    ]
    for name in qt_deps:
        src = vcpkg_bin / name
        if src.is_file():
            shutil.copy2(src, bin_dir / name)
            print(f"Copied Qt dep {name}")
        else:
            eprint(f"Optional Qt dep missing in vcpkg: {name}")

    if not (bin_dir / "platforms" / "qwindows.dll").is_file():
        raise SystemExit("platforms\\qwindows.dll missing after windeployqt")

    if args.shortcuts:
        want_all_users = not args.current_user_only
        is_admin = _is_admin()
        if want_all_users and not is_admin:
            eprint(
                "All Users Start Menu / Public Desktop need Administrator. "
                "Falling back to current-user shortcuts."
            )
            want_all_users = False

        shortcut_dirs: list[Path] = []
        if want_all_users:
            common_programs = _special_folder("CommonPrograms")
            common_desktop = _special_folder("CommonDesktopDirectory")
            if common_programs is not None:
                shortcut_dirs.append(common_programs / "ArchStreamer")
            if common_desktop is not None:
                shortcut_dirs.append(common_desktop)
            print("Installing All Users shortcuts (system-wide)...")
        else:
            appdata = os.environ.get("APPDATA", "")
            if appdata:
                shortcut_dirs.append(
                    Path(appdata) / "Microsoft" / "Windows" / "Start Menu" / "Programs" / "ArchStreamer"
                )
            user_desktop = _special_folder("Desktop")
            if user_desktop is not None:
                shortcut_dirs.append(user_desktop)
            print("Installing current-user shortcuts only...")

        for directory in shortcut_dirs:
            try:
                _create_shortcut(
                    directory / "ArchStreamer.lnk",
                    exe,
                    bin_dir,
                    "ArchStreamer",
                )
            except Exception as exc:
                eprint(f"Could not write shortcut under {directory} : {exc}")

        host_exe = bin_dir / "host_runner.exe"
        if host_exe.is_file() and want_all_users:
            common_programs = _special_folder("CommonPrograms")
            if common_programs is not None:
                host_dir = common_programs / "ArchStreamer"
                try:
                    _create_shortcut(
                        host_dir / "ArchStreamer Host (CLI).lnk",
                        host_exe,
                        bin_dir,
                        "ArchStreamer host_runner",
                    )
                except Exception as exc:
                    eprint(f"Could not write host_runner shortcut: {exc}")

    print("")
    print(f"Install ready: {exe}")
    print("GStreamer must still be on PATH (gst-launch-1.0).")
    if args.shortcuts:
        if not args.current_user_only and _is_admin():
            print("Start Menu (all users): Programs\\ArchStreamer\\ArchStreamer")
            print("Desktop (all users): Public Desktop\\ArchStreamer")
        else:
            print("Start Menu (this user): Programs\\ArchStreamer\\ArchStreamer")
    if args.launch:
        cmd = [str(exe)]
        if args.gui_branch:
            cmd.extend(["--branch", str(args.gui_branch).strip()])
        subprocess.Popen(cmd, cwd=str(bin_dir))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except subprocess.CalledProcessError as exc:
        raise SystemExit(exc.returncode) from exc
