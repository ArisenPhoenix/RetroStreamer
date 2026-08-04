#!/usr/bin/env python3
"""Windows dependency installer for ArchStreamer client + host.

Reference implementation: deploy/windows/install-deps.ps1
"""

from __future__ import annotations

import argparse
import os
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
    which,
)


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(
        description="Install ArchStreamer Windows dependencies (winget / vcpkg / GStreamer / ViGEm).",
        epilog="Reference implementation: deploy/windows/install-deps.ps1",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    p.add_argument(
        "--open-firewall",
        action="store_true",
        help="Open inbound firewall ports used by ArchStreamer (requires elevation)",
    )
    p.add_argument(
        "--install-build-tools",
        action="store_true",
        help="Install CMake/Ninja via winget when missing",
    )
    p.add_argument(
        "--vcpkg-root",
        type=Path,
        default=None,
        help="vcpkg root (default: VCPKG_ROOT or C:\\dev\\vcpkg)",
    )
    return p.parse_args()


def _is_admin() -> bool:
    try:
        import ctypes

        return bool(ctypes.windll.shell32.IsUserAnAdmin())  # type: ignore[attr-defined]
    except Exception:
        return False


def _ensure_winget() -> bool:
    if which("winget"):
        return True
    eprint("winget not found. Install 'App Installer' from the Microsoft Store, then re-run.")
    return False


def _add_user_path(directory: Path) -> None:
    if not directory.is_dir():
        return
    dir_str = str(directory)
    try:
        import winreg

        key = winreg.OpenKey(
            winreg.HKEY_CURRENT_USER,
            r"Environment",
            0,
            winreg.KEY_READ | winreg.KEY_WRITE,
        )
        try:
            user_path, _ = winreg.QueryValueEx(key, "Path")
        except FileNotFoundError:
            user_path = ""
        parts = [p for p in user_path.split(";") if p]
        if any(p.lower() == dir_str.lower() for p in parts):
            winreg.CloseKey(key)
            return
        new_path = f"{user_path};{dir_str}" if user_path else dir_str
        winreg.SetValueEx(key, "Path", 0, winreg.REG_EXPAND_SZ, new_path)
        winreg.CloseKey(key)
    except OSError as exc:
        eprint(f"Could not update user PATH: {exc}")
        return
    os.environ["Path"] = os.environ.get("Path", "") + ";" + dir_str
    print(f"Added to user PATH: {dir_str}")


def _winget_install(package_id: str) -> None:
    run(
        [
            "winget",
            "install",
            "-e",
            "--id",
            package_id,
            "--accept-package-agreements",
            "--accept-source-agreements",
        ],
        check=False,
    )


def _open_firewall_rules() -> None:
    rules = [
        ("ArchStreamer Control TCP", 45555, "TCP"),
        ("ArchStreamer Input UDP", 45454, "UDP"),
        ("ArchStreamer Video RTP", 5004, "UDP"),
        ("ArchStreamer Audio RTP", 6004, "UDP"),
        ("ArchStreamer LAN Advertise", 45550, "UDP"),
    ]
    for name, port, proto in rules:
        # Prefer New-NetFirewallRule; fall back to netsh.
        check = subprocess.run(
            [
                "powershell",
                "-NoProfile",
                "-Command",
                f"Get-NetFirewallRule -DisplayName '{name}' -ErrorAction SilentlyContinue",
            ],
            capture_output=True,
            text=True,
            check=False,
        )
        if check.returncode == 0 and check.stdout.strip():
            print(f"Firewall already present: {name}")
            continue

        create = subprocess.run(
            [
                "powershell",
                "-NoProfile",
                "-Command",
                (
                    f"New-NetFirewallRule -DisplayName '{name}' -Direction Inbound "
                    f"-Action Allow -Protocol {proto} -LocalPort {port} | Out-Null"
                ),
            ],
            capture_output=True,
            text=True,
            check=False,
        )
        if create.returncode == 0:
            print(f"Firewall allowed: {name} {proto}/{port}")
            continue

        # netsh fallback
        rule_name = name.replace(" ", "_")
        netsh = subprocess.run(
            [
                "netsh",
                "advfirewall",
                "firewall",
                "add",
                "rule",
                f"name={rule_name}",
                "dir=in",
                "action=allow",
                f"protocol={proto}",
                f"localport={port}",
            ],
            capture_output=True,
            text=True,
            check=False,
        )
        if netsh.returncode == 0:
            print(f"Firewall allowed (netsh): {name} {proto}/{port}")
        else:
            eprint(f"Failed to add firewall rule: {name}")
            if create.stderr:
                eprint(create.stderr.strip())
            if netsh.stderr:
                eprint(netsh.stderr.strip())


def main() -> int:
    args = parse_args()
    require_windows()
    vcpkg_root = Path(args.vcpkg_root) if args.vcpkg_root else default_vcpkg_root()

    print("=== ArchStreamer Windows deps ===")
    if not _is_admin():
        eprint(
            "Not elevated — ViGEmBus driver install may fail. "
            "Re-run in an Admin PowerShell / terminal."
        )

    # --- Build tools ---
    if not which("cmake"):
        print("CMake missing.")
        if args.install_build_tools and _ensure_winget():
            _winget_install("Kitware.CMake")
        else:
            print("  Install: winget install Kitware.CMake   (or pass --install-build-tools)")
    else:
        print("CMake: OK")

    if not which("ninja"):
        print("Ninja missing (optional but recommended).")
        if args.install_build_tools and _ensure_winget():
            _winget_install("Ninja-build.Ninja")
        else:
            print("  Install: winget install Ninja-build.Ninja")
    else:
        print("Ninja: OK")

    # --- vcpkg Qt/SDL2 ---
    toolchain = vcpkg_root / "scripts" / "buildsystems" / "vcpkg.cmake"
    if not toolchain.is_file():
        eprint(f"vcpkg not found at {vcpkg_root}")
        print("  Clone https://github.com/microsoft/vcpkg and bootstrap, or set VCPKG_ROOT.")
    else:
        print(f"vcpkg: {vcpkg_root}")
        vcpkg_exe = vcpkg_root / "vcpkg.exe"
        if vcpkg_exe.is_file():
            print("Installing vcpkg packages: qtbase[widgets], sdl2 ...")
            run(
                [vcpkg_exe, "install", "qtbase[widgets]:x64-windows", "sdl2:x64-windows"],
                check=False,
            )

    # --- GStreamer MSVC ---
    gst_launch = which("gst-launch-1.0.exe") or which("gst-launch-1.0")
    if not gst_launch:
        print("GStreamer MSVC not on PATH.")
        gst_urls = [
            "https://gstreamer.freedesktop.org/data/pkg/windows/1.24.12/msvc/gstreamer-1.0-msvc-x86_64-1.24.12.msi",
            "https://gstreamer.freedesktop.org/data/pkg/windows/1.24.12/msvc/gstreamer-1.0-devel-msvc-x86_64-1.24.12.msi",
        ]
        print("  Manual MSI downloads (runtime + devel):")
        for u in gst_urls:
            print(f"    {u}")
        print(
            "  After install, add e.g. "
            "C:\\Program Files\\gstreamer\\1.0\\msvc_x86_64\\bin to PATH."
        )
        if _ensure_winget():
            print("  Trying winget GStreamer packages (IDs vary by catalog)...")
            subprocess.run(
                ["winget", "search", "GStreamer"],
                check=False,
            )
    else:
        print(f"GStreamer: {gst_launch}")
        bin_dir = Path(gst_launch).resolve().parent
        _add_user_path(bin_dir)
        print("  Checking host plugins...")
        gst_inspect = which("gst-inspect-1.0.exe") or which("gst-inspect-1.0")
        for el in (
            "d3d11screencapturesrc",
            "wasapisrc",
            "x264enc",
            "opusenc",
            "multiudpsink",
        ):
            if not gst_inspect:
                eprint(f"    {el} missing — gst-inspect-1.0 not found")
                continue
            probe = subprocess.run(
                [gst_inspect, el],
                capture_output=True,
                text=True,
                check=False,
            )
            if probe.returncode == 0:
                print(f"    {el} OK")
            else:
                eprint(f"    {el} missing — install GStreamer plugins / complete package")

    # --- ViGEmBus ---
    vigem_ok = False
    try:
        svc = subprocess.run(
            [
                "powershell",
                "-NoProfile",
                "-Command",
                "Get-Service -Name 'ViGEmBus' -ErrorAction SilentlyContinue | "
                "Select-Object -ExpandProperty Status",
            ],
            capture_output=True,
            text=True,
            check=False,
        )
        status = (svc.stdout or "").strip()
        if svc.returncode == 0 and status:
            print(f"ViGEmBus service: {status}")
            vigem_ok = True
    except OSError:
        pass

    if not vigem_ok:
        print("ViGEmBus driver not detected.")
        if _ensure_winget():
            print("  Installing Nefarius ViGEmBus via winget...")
            _winget_install("Nefarius.ViGEmBus")
        else:
            print("  Download: https://github.com/nefarius/ViGEmBus/releases")
        print(
            "  Also place ViGEmClient.dll on PATH "
            "(from Nefarius.ViGEm.Client NuGet / SDK)."
        )

    # --- Yuzu (manual) ---
    local_appdata = os.environ.get("LOCALAPPDATA", "")
    yuzu_managed = (
        Path(local_appdata) / "archstreamer" / "yuzu" / "yuzu.exe"
        if local_appdata
        else Path()
    )
    print("")
    print("Yuzu is NOT auto-downloaded.")
    print("  Copy your yuzu-windows-msvc folder to:")
    if local_appdata:
        print(f"    {local_appdata}\\archstreamer\\yuzu\\")
    else:
        print("    %LOCALAPPDATA%\\archstreamer\\yuzu\\")
    print("  or set ARCHSTREAMER_YUZU to yuzu.exe (or its folder).")
    print("  Keys: %APPDATA%\\yuzu\\keys\\prod.keys  (or ARCHSTREAMER_YUZU_KEYS)")
    if yuzu_managed.is_file():
        print(f"  Managed yuzu.exe found: {yuzu_managed}")
    else:
        eprint("  Managed yuzu.exe not found yet.")

    # --- Firewall ---
    if args.open_firewall:
        if not _is_admin():
            eprint("--open-firewall requires elevation.")
        else:
            _open_firewall_rules()
    else:
        print("")
        print("Firewall (optional): re-run with --open-firewall, or:")
        print(
            '  New-NetFirewallRule -DisplayName "ArchStreamer Control TCP" '
            "-Direction Inbound -Action Allow -Protocol TCP -LocalPort 45555"
        )

    print("")
    print("Done. Build with:")
    print("  python deploy/build_windows.py --reconfigure")
    print("  # host-capable:")
    print(
        f'  cmake -S . -B build -DARCHSTREAMER_BUILD_HOST=ON '
        f'-DCMAKE_TOOLCHAIN_FILE="{toolchain}"'
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except subprocess.CalledProcessError as exc:
        raise SystemExit(exc.returncode) from exc
