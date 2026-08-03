#!/usr/bin/env python3
"""Fetch CHD-capable PS1 libretro cores into ~/.config/retroarch/cores."""

from __future__ import annotations

import argparse
import os
import shutil
import stat
import sys
import tempfile
import urllib.request
import zipfile
from pathlib import Path

from scriptutil import eprint


def fetch_core(name: str, base: str, core_dir: Path, tmp: Path) -> None:
    zip_name = f"{name}_libretro.so.zip"
    print(f"==> {name}")
    zip_path = tmp / zip_name
    urllib.request.urlretrieve(f"{base}/{zip_name}", zip_path)
    with zipfile.ZipFile(zip_path, "r") as zf:
        zf.extractall(core_dir)
    so_path = core_dir / f"{name}_libretro.so"
    if so_path.is_file() and os.name != "nt":
        so_path.chmod(so_path.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)


def main(argv: list[str] | None = None) -> int:
    home = Path.home()
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="Reference implementation: scripts/fetch-ps1-cores.sh",
    )
    parser.add_argument(
        "--core-dir",
        type=Path,
        default=Path(
            os.environ.get(
                "ARCHSTREAMER_CORE_DIR", str(home / ".config/retroarch/cores")
            )
        ),
    )
    parser.add_argument(
        "--system-dir",
        type=Path,
        default=Path(
            os.environ.get(
                "ARCHSTREAMER_SYSTEM_DIR", str(home / ".config/retroarch/system")
            )
        ),
    )
    parser.add_argument(
        "--arch",
        default=os.environ.get("ARCHSTREAMER_LIBRETRO_ARCH", "x86_64"),
    )
    parser.add_argument(
        "--bios",
        type=Path,
        default=Path(os.environ["ARCHSTREAMER_PS1_BIOS"])
        if os.environ.get("ARCHSTREAMER_PS1_BIOS")
        else None,
        help="PS1 BIOS path (env ARCHSTREAMER_PS1_BIOS)",
    )
    args = parser.parse_args(argv)

    core_dir: Path = args.core_dir
    system_dir: Path = args.system_dir
    core_dir.mkdir(parents=True, exist_ok=True)
    system_dir.mkdir(parents=True, exist_ok=True)

    base = f"https://buildbot.libretro.com/nightly/linux/{args.arch}/latest"
    with tempfile.TemporaryDirectory() as tmp_str:
        tmp = Path(tmp_str)
        fetch_core("pcsx_rearmed", base, core_dir, tmp)
        fetch_core("swanstation", base, core_dir, tmp)

    bios: Path | None = args.bios
    if bios is not None and bios.is_file():
        print(f"==> Installing BIOS from {bios}")
        for name in ("scph1001.bin", "SCPH1001.BIN", "scph5501.bin"):
            shutil.copy2(bios, system_dir / name)
    elif not (system_dir / "scph5501.bin").is_file() and not (
        system_dir / "scph1001.bin"
    ).is_file():
        print(f"Note: no PS1 BIOS in {system_dir}.")
        print(
            "Set ARCHSTREAMER_PS1_BIOS=/path/to/SCPH1001.BIN and re-run, or copy BIOS there."
        )

    print()
    print("Installed:")
    for name in ("pcsx_rearmed_libretro.so", "swanstation_libretro.so"):
        path = core_dir / name
        if path.is_file():
            st = path.stat()
            print(f"{path}  size={st.st_size}")
        else:
            eprint(f"missing: {path}")
    print(f"System dir: {system_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
