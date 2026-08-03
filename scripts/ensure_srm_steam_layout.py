#!/usr/bin/env python3
"""Ensure a minimal Steam userdata layout for Steam ROM Manager."""

from __future__ import annotations

import argparse
import json
import os
import shutil
import sys
from datetime import datetime
from pathlib import Path

def _read_srm_env(settings_path: Path) -> tuple[str, str]:
    settings = json.loads(settings_path.read_text())
    env = settings.get("environmentVariables", {})
    steam = env.get("steamDirectory") or ""
    accounts = env.get("userAccounts") or []
    account = str(accounts[0]) if accounts else ""
    return steam, account


def _best_account_id(steam_dir: Path) -> str:
    userdata = steam_dir / "userdata"
    best_id = ""
    best_score = -1
    if not userdata.is_dir():
        return best_id
    for account in userdata.iterdir():
        if not account.is_dir() or not account.name.isdigit() or account.name == "0":
            continue
        config = account / "config"
        shortcuts = config / "shortcuts.vdf"
        if not shortcuts.is_file():
            continue
        grid = config / "grid"
        grid_count = 0
        if grid.is_dir():
            grid_count = sum(
                1 for p in grid.iterdir() if p.is_file() or p.is_symlink()
            )
        score = grid_count * 1000 + shortcuts.stat().st_size
        if score > best_score:
            best_score = score
            best_id = account.name
    return best_id


def is_valid_shortcuts(path: Path) -> bool:
    if not path.is_file():
        return False
    data = path.read_bytes()
    return len(data) >= 13 and data.startswith(b"\x00shortcuts\x00")


def ensure_layout(
    steam_dir: Path | None = None,
    account_id: str | None = None,
) -> tuple[Path, str, Path]:
    """Create/validate Steam layout. Returns (steam_dir, account_id, shortcuts)."""
    home = Path.home()
    steam = steam_dir
    account = account_id or ""
    settings = home / ".config/steam-rom-manager/userData/userSettings.json"

    if (steam is None or not account) and settings.is_file():
        srm_steam, srm_account = _read_srm_env(settings)
        if steam is None or str(steam) == "":
            steam = Path(srm_steam) if srm_steam else None
        if not account:
            account = srm_account

    if steam is None or str(steam) == "":
        steam = Path(os.environ.get("ARCHSTREAMER_STEAM_DIR", str(home / ".local/share/Steam")))
    else:
        steam = Path(steam)

    if not account:
        account = os.environ.get("ARCHSTREAMER_STEAM_ACCOUNT_ID", "") or _best_account_id(
            steam
        )

    if not account:
        account = "0"
        print("No Steam userdata account found; using stub account id 0.")

    config_dir = steam / "userdata" / account / "config"
    grid_dir = config_dir / "grid"
    shortcuts = config_dir / "shortcuts.vdf"

    (steam / "steamapps").mkdir(parents=True, exist_ok=True)
    grid_dir.mkdir(parents=True, exist_ok=True)

    if is_valid_shortcuts(shortcuts):
        print("Steam shortcuts already present; leaving unchanged:")
        print(f"  {shortcuts} ({shortcuts.stat().st_size} bytes)")
    else:
        if shortcuts.exists():
            bak = shortcuts.with_name(
                f"shortcuts.vdf.bak-invalid-{datetime.now().strftime('%Y%m%d-%H%M%S')}"
            )
            shutil.copy2(shortcuts, bak)
            print("Invalid/empty shortcuts.vdf backed up to:")
            print(f"  {bak}")
        shortcuts.write_bytes(b"\x00shortcuts\x00\x08\x08")
        print(f"Created minimal valid shortcuts.vdf: {shortcuts}")

    print("Steam layout ready for SRM:")
    print(f"  steamDirectory: {steam}")
    print(f"  account:        {account}")
    print(f"  grid:           {grid_dir}")
    print(f"  shortcuts:      {shortcuts}")
    return steam, account, shortcuts


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="Reference implementation: scripts/ensure_srm_steam_layout.sh",
    )
    parser.add_argument(
        "--steam-dir",
        type=Path,
        default=Path(os.environ["ARCHSTREAMER_STEAM_DIR"])
        if os.environ.get("ARCHSTREAMER_STEAM_DIR")
        else None,
    )
    parser.add_argument(
        "--account-id",
        default=os.environ.get("ARCHSTREAMER_STEAM_ACCOUNT_ID") or None,
    )
    args = parser.parse_args(argv)
    ensure_layout(args.steam_dir, args.account_id)
    return 0


if __name__ == "__main__":
    sys.exit(main())
