#!/usr/bin/env python3
"""Fetch libretro cores from buildbot into ~/.config/retroarch/cores."""

from __future__ import annotations

import argparse
import os
import stat
import sys
import tempfile
import urllib.error
import urllib.request
import zipfile
from pathlib import Path

from scriptutil import eprint

DEFAULT_CORES = [
    # Handheld / cart
    "melonds",
    "sameboy",
    "vbam",
    # Consoles present under typical ROM trees
    "mupen64plus_next",
    "parallel_n64",
    "pcsx_rearmed",
    "swanstation",
    "pcsx2",
    "ppsspp",
    "dolphin",
    "citra",
    "nestopia",
    "fceumm",
    "bsnes",
    "snes9x2010",
    "genesis_plus_gx",
    "picodrive",
    "mednafen_pce_fast",
]


def cores_for_rom_tree(rom_root: Path) -> list[str]:
    wanted: list[str] = []
    if (rom_root / "GB").is_dir() or (rom_root / "GBC").is_dir():
        wanted.append("sameboy")
    if (rom_root / "GBA").is_dir():
        wanted.append("vbam")
    if (rom_root / "NDS").is_dir() or (rom_root / "DS").is_dir():
        wanted.append("melonds")
    if (rom_root / "N64").is_dir():
        wanted.extend(["mupen64plus_next", "parallel_n64"])
    if (rom_root / "PS1").is_dir() or (rom_root / "PSX").is_dir():
        wanted.extend(["pcsx_rearmed", "swanstation"])
    if (rom_root / "PS2").is_dir():
        wanted.append("pcsx2")
    if (rom_root / "PSP").is_dir():
        wanted.append("ppsspp")
    if (
        (rom_root / "GameCube").is_dir()
        or (rom_root / "GC").is_dir()
        or (rom_root / "Wii").is_dir()
    ):
        wanted.append("dolphin")
    if (rom_root / "3DS").is_dir():
        wanted.append("citra")
    if (rom_root / "SNES").is_dir() or (rom_root / "SFC").is_dir():
        wanted.extend(["bsnes", "snes9x2010"])
    if (rom_root / "NES").is_dir() or (rom_root / "Famicom").is_dir():
        wanted.extend(["nestopia", "fceumm"])
    if (
        (rom_root / "Genesis").is_dir()
        or (rom_root / "MegaDrive").is_dir()
        or (rom_root / "SMS").is_dir()
    ):
        wanted.extend(["genesis_plus_gx", "picodrive"])
    if (rom_root / "PCE").is_dir() or (rom_root / "TG16").is_dir():
        wanted.append("mednafen_pce_fast")
    seen: set[str] = set()
    out: list[str] = []
    for name in wanted:
        if name not in seen:
            seen.add(name)
            out.append(name)
    return out


def fetch_core(name: str, base: str, core_dir: Path, tmp: Path) -> None:
    zip_name = f"{name}_libretro.so.zip"
    so_name = f"{name}_libretro.so"
    so_path = core_dir / so_name
    if so_path.is_file():
        print(f"==> {name} (already present, refreshing)")
    else:
        print(f"==> {name}")

    zip_path = tmp / zip_name
    url = f"{base}/{zip_name}"
    try:
        urllib.request.urlretrieve(url, zip_path)
    except urllib.error.URLError:
        eprint(f"    skip: not on buildbot ({zip_name})")
        return

    with zipfile.ZipFile(zip_path, "r") as zf:
        zf.extractall(core_dir)

    if so_path.is_file():
        if os.name != "nt":
            so_path.chmod(so_path.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
        size = so_path.stat().st_size
        print(f"{so_path}  {size}")
    else:
        eprint(f"    warning: zip extracted but {so_name} not found")
        with zipfile.ZipFile(zip_path, "r") as zf:
            for info in zf.infolist()[:20]:
                print(f"    {info.filename}")


def main(argv: list[str] | None = None) -> int:
    home = Path.home()
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="Reference implementation: scripts/fetch-libretro-cores.sh",
    )
    parser.add_argument(
        "--catalog-systems",
        "--rom-tree",
        dest="catalog_systems",
        action="store_true",
        help="only systems under ROM root",
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
        "--rom-root",
        type=Path,
        default=Path(
            os.environ.get(
                "ARCHSTREAMER_ROM_ROOT", "<Gaming>/ROMS/Games"
            )
        ),
    )
    parser.add_argument(
        "--cores",
        nargs="*",
        default=None,
        help="optional explicit core list (overrides defaults / env)",
    )
    args = parser.parse_args(argv)

    core_dir: Path = args.core_dir
    system_dir: Path = args.system_dir
    core_dir.mkdir(parents=True, exist_ok=True)
    system_dir.mkdir(parents=True, exist_ok=True)

    if args.cores is not None and len(args.cores) > 0:
        cores = list(args.cores)
    elif args.catalog_systems:
        cores = cores_for_rom_tree(args.rom_root)
    elif os.environ.get("ARCHSTREAMER_CORES", "").strip():
        cores = os.environ["ARCHSTREAMER_CORES"].split()
    else:
        cores = list(DEFAULT_CORES)

    if not cores:
        eprint("No cores selected.")
        return 1

    base = f"https://buildbot.libretro.com/nightly/linux/{args.arch}/latest"
    print(f"Core dir: {core_dir}")
    print(f"Fetching: {' '.join(cores)}")
    print()

    with tempfile.TemporaryDirectory() as tmp_str:
        tmp = Path(tmp_str)
        for core in cores:
            fetch_core(core, base, core_dir, tmp)

    print()
    print(f"Done. Installed cores in {core_dir}:")
    installed = sorted(p.name for p in core_dir.glob("*_libretro.so"))
    for name in installed:
        print(name)

    print(
        f"""
Notes:
  - Restart ArchStreamer host to rescan the catalog (N64/PS2/PSP/etc. need these cores).
  - BIOS / firmware still required for some systems under {system_dir}:
      PS1:  scph5501.bin / scph1001.bin
      PS2:  SCPH-*.bin under {system_dir}/pcsx2/bios  (PCSX2 libretro)
      NDS:  bios7.bin + bios9.bin + firmware.bin (optional; melonDS freeBIOS works)
      PSP:  no real BIOS (PPSSPP HLE); assets under {system_dir}/PPSSPP
      3DS:  aes_keys.txt + seeddb.bin (Citra; also copied into saves/Citra/.../sysdata)
            melonDS cannot run 3DS — it is Nintendo DS only
      Switch: not shipped as a libretro core here (use standalone / ra.py Yuzu fallback)
  - Wire dumps from your BIOS library:
      ./scripts/link-system-bios.sh
      ARCHSTREAMER_PS1_BIOS=/path/to/SCPH1001.BIN ./scripts/fetch-ps1-cores.sh
"""
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
